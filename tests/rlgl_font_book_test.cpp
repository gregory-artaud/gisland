#include "gisland/rlgl_font_book.hpp"

#include "gisland/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    throw std::runtime_error{"failed to read " + path.string()};
  }
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] gisland::Theme load_theme() {
  const auto path = std::filesystem::path{GISLAND_TEST_ASSET_ROOT} / "themes/default.toml";
  return gisland::parse_theme(read_file(path), path.string()).value();
}

} // namespace

TEST_CASE("portable font atlas exposes deterministic glyph data") {
  const auto path = std::filesystem::path{GISLAND_TEST_ASSET_ROOT} / "fonts/Inter-Regular.ttf";
  constexpr std::array codepoints{0x20, 0x3F, 0x41, 0xE9};
  const auto atlas = gisland::build_font_atlas(path, 16, codepoints);

  REQUIRE(atlas.has_value());
  CHECK(atlas->base_size == 16);
  CHECK(atlas->padding == 4);
  CHECK(atlas->width > 0);
  CHECK(atlas->height > 0);
  CHECK(atlas->alpha.size() ==
        static_cast<std::size_t>(atlas->width) * static_cast<std::size_t>(atlas->height));
  REQUIRE(atlas->glyphs.size() == 4U);
  CHECK(atlas->glyphs[0].codepoint == U' ');
  CHECK(atlas->glyphs[0].advance_x > 0);
  CHECK(atlas->glyphs[2].codepoint == U'A');
  CHECK(atlas->glyphs[2].rectangle.width > 0);
}

TEST_CASE("portable font book matches gallery measurements and rejects unsupported glyphs") {
  const auto theme = load_theme();
  const auto fonts = gisland::RlglFontBook::load(theme, GISLAND_TEST_ASSET_ROOT);
  REQUIRE(fonts.has_value());
  CHECK(fonts->loaded_font_count() == 9U);

  const auto &title = theme.typography().at("title");
  const auto &body = theme.typography().at("body");
  const auto title_resource = theme.fonts().at(title.font);
  const auto body_resource = theme.fonts().at(body.font);
  const auto icon_resource = theme.fonts().at(theme.icons().at("calendar").font);

  CHECK(fonts->measure_text(title_resource, title, "Primitive gallery").width > 0.0);
  CHECK(fonts->measure_text(body_resource, body, "Text and icon").height == 19.2);
  CHECK(fonts->measure_codepoint(icon_resource, body, U'\uF133').width > 0.0);
  CHECK(fonts->supports_text(body_resource, body, "Muted"));
  CHECK_FALSE(fonts->supports_text(body_resource, body, "\u4F60\u597D"));
}
