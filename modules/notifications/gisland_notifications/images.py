import base64
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable
from urllib.parse import unquote, urlparse


MAX_DIMENSION = 512
MAX_DECODED_BYTES = 4 * 1024 * 1024


@dataclass(frozen=True)
class ImageData:
    width: int
    height: int
    pixels: bytes


def _downscale(image: ImageData, maximum: int = MAX_DIMENSION) -> ImageData:
    if image.width <= maximum and image.height <= maximum:
        return image
    scale = min(maximum / image.width, maximum / image.height)
    width = max(1, int(image.width * scale))
    height = max(1, int(image.height * scale))
    pixels = bytearray(width * height * 4)
    for target_y in range(height):
        source_y = min(image.height - 1, target_y * image.height // height)
        for target_x in range(width):
            source_x = min(image.width - 1, target_x * image.width // width)
            source = (source_y * image.width + source_x) * 4
            target = (target_y * width + target_x) * 4
            pixels[target : target + 4] = image.pixels[source : source + 4]
    return ImageData(width, height, bytes(pixels))


def normalize_raw_image(raw: Any) -> ImageData:
    if not isinstance(raw, (tuple, list)) or len(raw) != 7:
        raise ValueError("image data must be a seven-field tuple")
    width, height, rowstride, has_alpha, bits_per_sample, channels, source = raw
    if not all(isinstance(value, int) and not isinstance(value, bool) for value in (width, height, rowstride, bits_per_sample, channels)):
        raise ValueError("image dimensions and format fields must be integers")
    if not isinstance(has_alpha, bool):
        raise ValueError("image alpha flag must be boolean")
    if width <= 0 or height <= 0 or width > 16384 or height > 16384:
        raise ValueError("image dimensions are invalid")
    if bits_per_sample != 8 or channels not in (3, 4) or has_alpha != (channels == 4):
        raise ValueError("only RGB8 and RGBA8 image data are supported")
    minimum_stride = width * channels
    if rowstride < minimum_stride:
        raise ValueError("image rowstride is too small")
    data = bytes(source)
    required = rowstride * height
    if len(data) != required or required > 256 * 1024 * 1024:
        raise ValueError("image byte count does not match its rowstride")

    pixels = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
            source_index = y * rowstride + x * channels
            target_index = (y * width + x) * 4
            pixels[target_index : target_index + 3] = data[source_index : source_index + 3]
            pixels[target_index + 3] = data[source_index + 3] if has_alpha else 255
    return _downscale(ImageData(width, height, bytes(pixels)))


def encode_resource(resource_id: str, image: ImageData) -> dict[str, Any]:
    if len(image.pixels) != image.width * image.height * 4:
        raise ValueError("RGBA8 image has an invalid byte count")
    if len(image.pixels) > MAX_DECODED_BYTES:
        raise ValueError("RGBA8 image exceeds the protocol resource limit")
    return {
        "id": resource_id,
        "format": "rgba8",
        "width": image.width,
        "height": image.height,
        "data": base64.b64encode(image.pixels).decode("ascii"),
    }


def _local_path(value: str) -> Path:
    parsed = urlparse(value)
    if parsed.scheme and parsed.scheme != "file":
        raise ValueError("remote image sources are unsupported")
    if parsed.scheme == "file":
        if parsed.netloc not in ("", "localhost"):
            raise ValueError("non-local file URI is unsupported")
        return Path(unquote(parsed.path))
    return Path(value)


def load_image_file(value: str) -> ImageData:
    path = _local_path(value)
    if not path.is_file():
        raise ValueError(f"image does not exist: {path}")
    import gi

    gi.require_version("GdkPixbuf", "2.0")
    from gi.repository import GdkPixbuf

    try:
        pixbuf = GdkPixbuf.Pixbuf.new_from_file(str(path))
    except Exception as error:
        raise ValueError(f"image decode failed: {error}") from error
    data = bytes(pixbuf.read_pixel_bytes().get_data())
    return normalize_raw_image(
        (
            pixbuf.get_width(),
            pixbuf.get_height(),
            pixbuf.get_rowstride(),
            pixbuf.get_has_alpha(),
            pixbuf.get_bits_per_sample(),
            pixbuf.get_n_channels(),
            data,
        )
    )


def load_icon_name(name: str) -> ImageData:
    import gi

    gi.require_version("Gtk", "3.0")
    from gi.repository import Gtk

    theme = Gtk.IconTheme.get_default() or Gtk.IconTheme.new()
    icon = theme.lookup_icon(name, MAX_DIMENSION, Gtk.IconLookupFlags.FORCE_SVG)
    if icon is None or icon.get_filename() is None:
        raise ValueError(f"icon was not found: {name}")
    return load_image_file(icon.get_filename())


def resolve_app_image(
    hints: dict[str, Any],
    app_icon: str,
    path_loader: Callable[[str], ImageData] = load_image_file,
    icon_loader: Callable[[str], ImageData] = load_icon_name,
) -> ImageData | None:
    for key in ("image-data", "image_data", "icon_data"):
        if key in hints:
            try:
                return normalize_raw_image(hints[key])
            except (TypeError, ValueError):
                break
    for key in ("image-path", "image_path"):
        value = hints.get(key)
        if isinstance(value, str) and value:
            try:
                return path_loader(value)
            except (OSError, ValueError):
                break
    if not app_icon:
        return None
    try:
        if app_icon.startswith("file:") or "/" in app_icon:
            return path_loader(app_icon)
        return icon_loader(app_icon)
    except (OSError, ValueError):
        return None
