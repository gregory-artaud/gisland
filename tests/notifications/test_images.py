import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from gisland_notifications.images import (
    ImageData,
    encode_resource,
    load_desktop_entry_icon,
    load_image_file,
    normalize_raw_image,
    resolve_app_image,
)


class RawImageTests(unittest.TestCase):
    def test_validates_stride_and_converts_padded_rgb_to_rgba(self):
        image = normalize_raw_image((2, 1, 8, False, 8, 3, bytes([1, 2, 3, 4, 5, 6, 0, 0])))

        self.assertEqual(image, ImageData(2, 1, bytes([1, 2, 3, 255, 4, 5, 6, 255])))

    def test_rejects_malformed_raw_image_tuples(self):
        invalid = [
            (0, 1, 4, True, 8, 4, b""),
            (1, 1, 3, True, 8, 4, b"\0" * 4),
            (1, 1, 4, True, 16, 4, b"\0" * 4),
            (1, 1, 4, False, 8, 4, b"\0" * 4),
            (1, 2, 4, True, 8, 4, b"\0" * 4),
        ]

        for raw in invalid:
            with self.subTest(raw=raw):
                with self.assertRaises(ValueError):
                    normalize_raw_image(raw)

    def test_encodes_straight_rgba_resource(self):
        resource = encode_resource("app-image", ImageData(1, 1, bytes([255, 0, 0, 128])))

        self.assertEqual(resource["format"], "rgba8")
        self.assertEqual(resource["data"], "/wAAgA==")


class ImageResolutionTests(unittest.TestCase):
    def test_uses_hint_priority_and_falls_back_after_invalid_source(self):
        requested = []

        def path_loader(value):
            requested.append(("path", value))
            if value == "broken.png":
                raise ValueError("broken")
            return ImageData(1, 1, b"\x01\x02\x03\xff")

        def icon_loader(value):
            requested.append(("icon", value))
            return ImageData(1, 1, b"\x04\x05\x06\xff")

        image = resolve_app_image(
            {"image-data": (0, 1, 4, True, 8, 4, b""), "image-path": "broken.png"},
            "folder",
            path_loader=path_loader,
            icon_loader=icon_loader,
        )

        self.assertEqual(image.pixels, b"\x04\x05\x06\xff")
        self.assertEqual(requested, [("path", "broken.png"), ("icon", "folder")])

    def test_resolves_case_insensitive_desktop_entry_icon_from_xdg_data(self):
        with TemporaryDirectory() as root:
            applications = Path(root) / "applications"
            applications.mkdir()
            (applications / "slack.desktop").write_text(
                "[Desktop Entry]\nName=Slack\nIcon=slack\nType=Application\n",
                encoding="utf-8",
            )
            requested = []

            image = load_desktop_entry_icon(
                "Slack",
                data_dirs=(Path(root),),
                icon_loader=lambda value: requested.append(value)
                or ImageData(1, 1, b"\x01\x02\x03\xff"),
            )

        self.assertEqual(image.pixels, b"\x01\x02\x03\xff")
        self.assertEqual(requested, ["slack"])

    def test_desktop_entry_is_the_final_app_image_fallback(self):
        requested = []
        desktop_image = ImageData(1, 1, b"\x07\x08\x09\xff")

        from_desktop = resolve_app_image(
            {"desktop-entry": "Slack"},
            "",
            desktop_loader=lambda value: requested.append(("desktop", value)) or desktop_image,
        )
        from_app_icon = resolve_app_image(
            {"desktop-entry": "Slack"},
            "folder",
            icon_loader=lambda value: requested.append(("icon", value))
            or ImageData(1, 1, b"\x04\x05\x06\xff"),
            desktop_loader=lambda value: requested.append(("desktop", value)) or desktop_image,
        )

        self.assertEqual(from_desktop, desktop_image)
        self.assertEqual(from_app_icon.pixels, b"\x04\x05\x06\xff")
        self.assertEqual(requested, [("desktop", "Slack"), ("icon", "folder")])

    def test_rejects_unsafe_or_icon_free_desktop_entries(self):
        with TemporaryDirectory() as root:
            applications = Path(root) / "applications"
            applications.mkdir()
            (applications / "empty.desktop").write_text(
                "[Desktop Entry]\nName=Empty\nType=Application\n", encoding="utf-8"
            )

            with self.assertRaises(ValueError):
                load_desktop_entry_icon("../slack", data_dirs=(Path(root),))
            with self.assertRaises(ValueError):
                load_desktop_entry_icon("empty", data_dirs=(Path(root),))

    def test_rejects_remote_image_paths(self):
        with self.assertRaises(ValueError):
            load_image_file("https://example.com/image.png")

    def test_loads_a_png_through_gdk_pixbuf(self):
        baseline = Path(__file__).resolve().parents[1] / "baselines/notification-compact.png"
        image = load_image_file(str(baseline))

        self.assertEqual((image.width, image.height), (480, 360))
        self.assertEqual(len(image.pixels), 480 * 360 * 4)


if __name__ == "__main__":
    unittest.main()
