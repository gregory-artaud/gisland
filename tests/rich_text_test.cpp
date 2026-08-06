#include "gisland/rich_text.hpp"
#include "gisland/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream stream{path};
  REQUIRE(stream.is_open());
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::filesystem::path asset_root() { return GISLAND_TEST_ASSET_ROOT; }

[[nodiscard]] gisland::PangoTextBook load_book() {
  const auto theme_text = read_file(asset_root() / "themes/default.toml");
  const auto theme = gisland::parse_theme(theme_text, "default.toml");
  REQUIRE(theme.has_value());

  auto book = gisland::PangoTextBook::load(*theme, asset_root());
  REQUIRE(book.has_value());
  return std::move(*book);
}

} // namespace

TEST_CASE("PangoCairo composition wraps rich Unicode text at its assigned width") {
  auto book = load_book();
  const gisland::RichText rich{
      .role = "body",
      .content =
          {
              gisland::RichTextSpan{"Cafe \xC3\xA9 and calendar ao\xC3\xBBt ",
                                    {gisland::TextEmphasis::italic}},
              gisland::RichTextSpan{
                  "with emphasized words that wrap",
                  {gisland::TextEmphasis::bold, gisland::TextEmphasis::underline}},
          },
  };

  const auto narrow = book.compose(rich, 120);
  const auto wide = book.compose(rich, 420);

  REQUIRE(narrow.has_value());
  REQUIRE(wide.has_value());
  CHECK(narrow->assigned_width == 120);
  CHECK(narrow->line_count > wide->line_count);
  CHECK(narrow->height > wide->height);
  CHECK(narrow->natural_width > narrow->minimum_width);
  CHECK(narrow->unknown_glyphs == 0);
}

TEST_CASE("PangoCairo composition preserves newlines and emits wrapped link rectangles") {
  auto book = load_book();
  const gisland::RichText rich{
      .role = "body",
      .content =
          {
              gisland::RichTextSpan{"First line\n", {}},
              gisland::RichLinkSpan{"Open the folder containing the downloaded archive",
                                    {},
                                    "link-0",
                                    "Open download folder"},
          },
  };

  const auto composition = book.compose(rich, 100);

  REQUIRE(composition.has_value());
  CHECK(composition->line_count >= 3);
  REQUIRE(composition->links.size() >= 2);
  for (const auto &link : composition->links) {
    CHECK(link.action_id == "link-0");
    CHECK(link.accessible_label == "Open download folder");
    CHECK(link.bounds.width > 0);
    CHECK(link.bounds.height > 0);
  }
}

TEST_CASE("PangoCairo composition reserves exact inline image role geometry") {
  auto book = load_book();
  const gisland::RichText rich{
      .role = "body",
      .content =
          {
              gisland::RichTextSpan{"Preview ", {}},
              gisland::RichInlineImage{"preview", "notification-icon", "Image preview"},
              gisland::RichTextSpan{" complete", {}},
          },
  };

  const auto composition = book.compose(rich, 320);

  REQUIRE(composition.has_value());
  REQUIRE(composition->images.size() == 1);
  CHECK(composition->images[0].resource_id == "preview");
  CHECK(composition->images[0].accessible_label == "Image preview");
  CHECK(composition->images[0].bounds.width == 24);
  CHECK(composition->images[0].bounds.height == 24);
}

TEST_CASE("Cairo premultiplied ARGB converts to straight RGBA8") {
  const std::vector<std::uint8_t> native = std::endian::native == std::endian::little
                                               ? std::vector<std::uint8_t>{16, 32, 64, 128}
                                               : std::vector<std::uint8_t>{128, 64, 32, 16};

  const auto converted = gisland::cairo_argb32_to_rgba(native, 1, 1, 4);

  REQUIRE(converted.has_value());
  CHECK(*converted == std::vector<std::uint8_t>{128, 64, 32, 128});
}

TEST_CASE("PangoCairo rasterization produces transparent straight RGBA text") {
  auto book = load_book();
  const gisland::RichText rich{
      .role = "body",
      .content = {gisland::RichTextSpan{"Rendered text", {gisland::TextEmphasis::bold}}},
  };

  const auto surface = book.rasterize(rich, 180, {});

  REQUIRE(surface.has_value());
  CHECK(surface->width == 180);
  CHECK(surface->height == surface->composition.height);
  REQUIRE(surface->pixels.size() == static_cast<std::size_t>(surface->width * surface->height * 4));
  CHECK(std::ranges::any_of(surface->pixels, [](std::uint8_t value) { return value != 0; }));
  CHECK(surface->pixels[3] == 0);
}

TEST_CASE("PangoCairo rasterization composites and clips inline RGBA resources") {
  auto book = load_book();
  const gisland::RichText rich{
      .role = "body",
      .content = {gisland::RichInlineImage{"preview", "notification-icon", "Preview"}},
  };
  const gisland::ImageResource resource{
      .id = "preview",
      .format = gisland::ImageFormat::rgba8,
      .width = 1,
      .height = 1,
      .pixels = std::make_shared<const std::vector<std::uint8_t>>(
          std::vector<std::uint8_t>{255, 0, 0, 255}),
  };

  const auto surface = book.rasterize(rich, 48, {resource});

  REQUIRE(surface.has_value());
  REQUIRE(surface->composition.images.size() == 1);
  const auto bounds = surface->composition.images[0].bounds;
  const auto pixel = [&surface](int x, int y) {
    const auto index = static_cast<std::size_t>((y * surface->width + x) * 4);
    return std::span<const std::uint8_t, 4>{surface->pixels.data() + index, 4};
  };
  CHECK(pixel(bounds.x, bounds.y)[3] == 0);
  const auto center = pixel(bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
  CHECK(center[0] == 255);
  CHECK(center[1] == 0);
  CHECK(center[2] == 0);
  CHECK(center[3] == 255);
}
