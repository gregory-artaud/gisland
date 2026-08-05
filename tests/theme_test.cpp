#include "gisland/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace {

constexpr std::string_view valid_theme = R"(
[palette]
surface = "#000000"
foreground = "#F4F4F580"
muted = "#A1A1AA"
accent = "#7C3AED"
success = "#22C55E"
warning = "#F59E0B"
error = "#EF4444"

[fonts]
ui = "/usr/share/fonts/ui.ttf"
symbols = "/usr/share/fonts/symbols.ttf"

[typography.body]
font = "ui"
size = 16
weight = 450
line_height = 1.25

[typography.title]
font = "ui"
color = "muted"
size = 24.5

[gaps]
normal = 8
small = 4.5

[spacers]
normal = 12

[view.compact]
padding = 10
radius = 18
border = 1
min_width = 120
max_width = 640
min_height = 36
max_height = 160

[view.expanded]
padding = 18
radius = 24
border = 1
min_width = 280
max_width = 960
min_height = 180
max_height = 720

[shadow]
offset_x = 0
offset_y = 6
blur = 18
spread = 0
color = "muted"

[animation]
compact_to_expanded_ms = 350
context_change_ms = 250
easing = "ease-in-out"

[animation.reduced_motion]
compact_to_expanded_ms = 0
context_change_ms = 0

[icons.calendar]
font = "symbols"
codepoint = 0xE001
)";

std::string replace_once(std::string source, std::string_view needle,
                         std::string_view replacement) {
  const auto position = source.find(needle);
  REQUIRE(position != std::string::npos);
  source.replace(position, needle.size(), replacement);
  return source;
}

} // namespace

TEST_CASE("theme TOML parses into typed semantic values") {
  const auto result = gisland::parse_theme(valid_theme, "theme.toml");

  REQUIRE(result.has_value());
  CHECK(result->palette().at("surface") == gisland::Rgba{0, 0, 0, 255});
  CHECK(result->palette().at("foreground") == gisland::Rgba{244, 244, 245, 128});
  CHECK(result->typography().at("body").font == "ui");
  CHECK(result->typography().at("body").color == "foreground");
  CHECK(result->typography().at("body").size == 16.0);
  CHECK(result->typography().at("body").weight == 450);
  CHECK(result->typography().at("body").line_height == 1.25);
  CHECK(result->typography().at("title").color == "muted");
  CHECK(result->gaps().at("small") == 4.5);
  CHECK(result->spacers().at("normal") == 12.0);
  CHECK(result->views().compact.padding_horizontal == 10.0);
  CHECK(result->views().compact.padding_vertical == 10.0);
  CHECK(result->views().compact.min_width == 120.0);
  CHECK(result->views().expanded.max_height == 720.0);
  CHECK(result->shadow().offset_x == 0.0);
  CHECK(result->shadow().offset_y == 6.0);
  CHECK(result->shadow().blur == 18.0);
  CHECK(result->shadow().spread == 0.0);
  REQUIRE(std::holds_alternative<std::string>(result->shadow().color));
  CHECK(std::get<std::string>(result->shadow().color) == "muted");
  CHECK(result->animation().compact_to_expanded_ms == std::chrono::milliseconds{350});
  CHECK(result->animation().context_change_ms == std::chrono::milliseconds{250});
  CHECK(result->animation().easing == gisland::Easing::ease_in_out);
  CHECK(result->animation().reduced_motion.compact_to_expanded_ms == std::chrono::milliseconds{0});
  CHECK(result->animation().reduced_motion.context_change_ms == std::chrono::milliseconds{0});
  CHECK(result->fonts().at("ui") == "/usr/share/fonts/ui.ttf");
  CHECK(result->icons().at("calendar").codepoint == U'\uE001');
}

TEST_CASE("theme parses an explicit RGBA shadow color") {
  const auto result =
      gisland::parse_theme(replace_once(std::string{valid_theme}, "spread = 0\ncolor = \"muted\"",
                                        "spread = 0\ncolor = \"#10203040\""),
                           "rgba-shadow.toml");

  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<gisland::Rgba>(result->shadow().color));
  CHECK(std::get<gisland::Rgba>(result->shadow().color) == gisland::Rgba{16, 32, 48, 64});
}

TEST_CASE("theme accepts each supported easing") {
  const auto check_easing = [](std::string_view name, gisland::Easing expected) {
    const auto result =
        gisland::parse_theme(replace_once(std::string{valid_theme}, "easing = \"ease-in-out\"",
                                          "easing = \"" + std::string{name} + "\""),
                             "easing.toml");
    REQUIRE(result.has_value());
    CHECK(result->animation().easing == expected);
  };

  check_easing("linear", gisland::Easing::linear);
  check_easing("ease-in", gisland::Easing::ease_in);
  check_easing("ease-out", gisland::Easing::ease_out);
  check_easing("ease-in-out", gisland::Easing::ease_in_out);
}

TEST_CASE("theme errors retain source position and a semantic path") {
  const auto result = gisland::parse_theme("[palette\nsurface = \"#000000\"", "broken.toml");

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().source == "broken.toml");
  CHECK_FALSE(result.error().message.empty());
  CHECK(result.error().line > 0);
  CHECK(result.error().column > 0);
}

TEST_CASE("theme rejects unknown keys at every fixed and dynamic table") {
  const auto check_unknown = [](const std::string &text, std::string_view path) {
    const auto result = gisland::parse_theme(text, "unknown.toml");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == path);
    CHECK(result.error().message == "unknown key");
  };

  check_unknown("extra = 1\n" + std::string{valid_theme}, "extra");
  check_unknown(replace_once(std::string{valid_theme}, "surface = \"#000000\"",
                             "surface = \"#000000\"\nother = \"#FFFFFF\""),
                "palette.other");
  check_unknown(replace_once(std::string{valid_theme}, "line_height = 1.25",
                             "line_height = 1.25\nslant = \"italic\""),
                "typography.body.slant");
  check_unknown(
      replace_once(std::string{valid_theme}, "max_height = 160", "max_height = 160\nshadow = 2"),
      "view.compact.shadow");
  check_unknown(replace_once(std::string{valid_theme}, "codepoint = 0xE001",
                             "codepoint = 0xE001\nlabel = \"Calendar\""),
                "icons.calendar.label");
  check_unknown(replace_once(std::string{valid_theme}, "spread = 0", "spread = 0\nopacity = 0.5"),
                "shadow.opacity");
  check_unknown(replace_once(std::string{valid_theme}, "easing = \"ease-in-out\"",
                             "easing = \"ease-in-out\"\ndelay_ms = 20"),
                "animation.delay_ms");
  check_unknown(replace_once(std::string{valid_theme}, "context_change_ms = 0\n\n[icons.calendar]",
                             "context_change_ms = 0\nextra = 1\n\n[icons.calendar]"),
                "animation.reduced_motion.extra");
}

TEST_CASE("theme rejects malformed colors and invalid bounded numbers") {
  const auto malformed = gisland::parse_theme(
      replace_once(std::string{valid_theme}, "#000000", "black"), "color.toml");
  REQUIRE_FALSE(malformed.has_value());
  CHECK(malformed.error().path == "palette.surface");

  const auto nonfinite = gisland::parse_theme(
      replace_once(std::string{valid_theme}, "small = 4.5", "small = inf"), "gap.toml");
  REQUIRE_FALSE(nonfinite.has_value());
  CHECK(nonfinite.error().path == "gaps.small");

  const auto negative = gisland::parse_theme(
      replace_once(std::string{valid_theme}, "normal = 12", "normal = -1"), "spacer.toml");
  REQUIRE_FALSE(negative.has_value());
  CHECK(negative.error().path == "spacers.normal");

  const auto oversized = gisland::parse_theme(
      replace_once(std::string{valid_theme}, "size = 16", "size = 513"), "type.toml");
  REQUIRE_FALSE(oversized.has_value());
  CHECK(oversized.error().path == "typography.body.size");
}

TEST_CASE("theme rejects invalid shadow values with exact paths") {
  const auto check_invalid = [](const std::string &text, std::string_view path) {
    const auto result = gisland::parse_theme(text, "shadow.toml");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == path);
  };

  check_invalid(replace_once(std::string{valid_theme}, "offset_x = 0", "offset_x = -16385"),
                "shadow.offset_x");
  check_invalid(replace_once(std::string{valid_theme}, "offset_y = 6", "offset_y = 16385"),
                "shadow.offset_y");
  check_invalid(replace_once(std::string{valid_theme}, "blur = 18", "blur = -1"), "shadow.blur");
  check_invalid(replace_once(std::string{valid_theme}, "spread = 0", "spread = 16385"),
                "shadow.spread");
  check_invalid(replace_once(std::string{valid_theme}, "spread = 0\ncolor = \"muted\"",
                             "spread = 0\ncolor = \"#xyz\""),
                "shadow.color");
  check_invalid(replace_once(std::string{valid_theme}, "spread = 0\ncolor = \"muted\"",
                             "spread = 0\ncolor = \"missing\""),
                "shadow.color");
  check_invalid(replace_once(std::string{valid_theme}, "spread = 0\ncolor = \"muted\"",
                             "spread = 0\ncolor = 1"),
                "shadow.color");
}

TEST_CASE("theme rejects invalid animation values with exact paths") {
  const auto check_invalid = [](const std::string &text, std::string_view path) {
    const auto result = gisland::parse_theme(text, "animation.toml");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == path);
  };

  check_invalid(replace_once(std::string{valid_theme}, "compact_to_expanded_ms = 350",
                             "compact_to_expanded_ms = -1"),
                "animation.compact_to_expanded_ms");
  check_invalid(replace_once(std::string{valid_theme}, "context_change_ms = 250",
                             "context_change_ms = 60001"),
                "animation.context_change_ms");
  check_invalid(replace_once(std::string{valid_theme}, "compact_to_expanded_ms = 350",
                             "compact_to_expanded_ms = 350.0"),
                "animation.compact_to_expanded_ms");
  check_invalid(
      replace_once(std::string{valid_theme}, "easing = \"ease-in-out\"", "easing = \"ease\""),
      "animation.easing");
  check_invalid(
      replace_once(std::string{valid_theme},
                   "compact_to_expanded_ms = 0\ncontext_change_ms = 0\n\n[icons.calendar]",
                   "compact_to_expanded_ms = 60001\ncontext_change_ms = 0\n\n[icons.calendar]"),
      "animation.reduced_motion.compact_to_expanded_ms");
  check_invalid(replace_once(std::string{valid_theme}, "context_change_ms = 0\n\n[icons.calendar]",
                             "context_change_ms = -1\n\n[icons.calendar]"),
                "animation.reduced_motion.context_change_ms");
}

TEST_CASE("theme rejects empty or duplicate semantic keys") {
  const auto empty = gisland::parse_theme(
      replace_once(std::string{valid_theme}, "[icons.calendar]", "[icons.\"\"]"), "empty.toml");
  REQUIRE_FALSE(empty.has_value());
  CHECK(empty.error().path == "icons");

  const auto duplicate = gisland::parse_theme(std::string{valid_theme} +
                                                  "\n[typography.body]\nfont = \"ui\"\nsize = 18\n",
                                              "duplicate.toml");
  REQUIRE_FALSE(duplicate.has_value());
  CHECK_FALSE(duplicate.error().message.empty());
}

TEST_CASE("theme validates view geometry relationships") {
  const auto result = gisland::parse_theme(
      replace_once(std::string{valid_theme}, "max_width = 640", "max_width = 100"),
      "geometry.toml");

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().path == "view.compact.max_width");
}

TEST_CASE("theme resolves explicit horizontal and vertical view padding") {
  const auto result =
      gisland::parse_theme(replace_once(std::string{valid_theme}, "padding = 10",
                                        "padding_horizontal = 14\npadding_vertical = 4"),
                           "axis-padding.toml");

  REQUIRE(result.has_value());
  CHECK(result->views().compact.padding_horizontal == 14.0);
  CHECK(result->views().compact.padding_vertical == 4.0);
}

TEST_CASE("theme rejects mixed and incomplete view padding forms") {
  const auto mixed = gisland::parse_theme(
      replace_once(std::string{valid_theme}, "padding = 10",
                   "padding = 10\npadding_horizontal = 14\npadding_vertical = 4"),
      "mixed-padding.toml");
  REQUIRE_FALSE(mixed.has_value());
  CHECK(mixed.error().path == "view.compact.padding_horizontal");

  const auto missing_vertical = gisland::parse_theme(
      replace_once(std::string{valid_theme}, "padding = 10", "padding_horizontal = 14"),
      "missing-padding.toml");
  REQUIRE_FALSE(missing_vertical.has_value());
  CHECK(missing_vertical.error().path == "view.compact.padding_vertical");

  const auto missing_horizontal = gisland::parse_theme(
      replace_once(std::string{valid_theme}, "padding = 10", "padding_vertical = 4"),
      "missing-padding.toml");
  REQUIRE_FALSE(missing_horizontal.has_value());
  CHECK(missing_horizontal.error().path == "view.compact.padding_horizontal");
}

TEST_CASE("theme validates horizontal and vertical padding against their own axes") {
  const auto horizontal =
      gisland::parse_theme(replace_once(std::string{valid_theme}, "padding = 10",
                                        "padding_horizontal = 60\npadding_vertical = 4"),
                           "horizontal-padding.toml");
  REQUIRE_FALSE(horizontal.has_value());
  CHECK(horizontal.error().path == "view.compact.padding_horizontal");

  const auto vertical =
      gisland::parse_theme(replace_once(std::string{valid_theme}, "padding = 10",
                                        "padding_horizontal = 14\npadding_vertical = 18"),
                           "vertical-padding.toml");
  REQUIRE_FALSE(vertical.has_value());
  CHECK(vertical.error().path == "view.compact.padding_vertical");
}

TEST_CASE("theme references only declared font resources and scalar codepoints") {
  const auto typography = gisland::parse_theme(
      replace_once(std::string{valid_theme}, "font = \"ui\"", "font = \"missing\""), "font.toml");
  REQUIRE_FALSE(typography.has_value());
  CHECK(typography.error().path == "typography.body.font");

  const auto icon_font = gisland::parse_theme(
      replace_once(std::string{valid_theme}, "font = \"symbols\"", "font = \"missing\""),
      "icon-font.toml");
  REQUIRE_FALSE(icon_font.has_value());
  CHECK(icon_font.error().path == "icons.calendar.font");

  const auto color = gisland::parse_theme(
      replace_once(std::string{valid_theme}, "color = \"muted\"", "color = \"missing\""),
      "type-color.toml");
  REQUIRE_FALSE(color.has_value());
  CHECK(color.error().path == "typography.title.color");

  const auto surrogate = gisland::parse_theme(
      replace_once(std::string{valid_theme}, "codepoint = 0xE001", "codepoint = 0xD800"),
      "codepoint.toml");
  REQUIRE_FALSE(surrogate.has_value());
  CHECK(surrogate.error().path == "icons.calendar.codepoint");
}

TEST_CASE("theme requires canonical roles tokens and both views") {
  const auto palette = gisland::parse_theme(
      replace_once(std::string{valid_theme}, "error = \"#EF4444\"", ""), "palette.toml");
  REQUIRE_FALSE(palette.has_value());
  CHECK(palette.error().path == "palette.error");

  const auto typography = gisland::parse_theme(
      replace_once(std::string{valid_theme}, "[typography.body]", "[typography.copy]"),
      "typography.toml");
  REQUIRE_FALSE(typography.has_value());
  CHECK(typography.error().path == "typography.body");

  const auto gap = gisland::parse_theme(
      replace_once(std::string{valid_theme}, "normal = 8", "standard = 8"), "gaps.toml");
  REQUIRE_FALSE(gap.has_value());
  CHECK(gap.error().path == "gaps.normal");

  const auto view = gisland::parse_theme(
      std::string{valid_theme.substr(0, valid_theme.find("[view.expanded]"))}, "view.toml");
  REQUIRE_FALSE(view.has_value());
  CHECK(view.error().path == "view.expanded");

  const auto shadow = gisland::parse_theme(
      std::string{valid_theme.substr(0, valid_theme.find("[shadow]"))}, "shadow.toml");
  REQUIRE_FALSE(shadow.has_value());
  CHECK(shadow.error().path == "shadow");

  const auto animation = gisland::parse_theme(
      replace_once(std::string{valid_theme},
                   "[animation]\ncompact_to_expanded_ms = 350\ncontext_change_ms = 250\n"
                   "easing = \"ease-in-out\"\n\n[animation.reduced_motion]\n"
                   "compact_to_expanded_ms = 0\ncontext_change_ms = 0\n\n",
                   ""),
      "animation.toml");
  REQUIRE_FALSE(animation.has_value());
  CHECK(animation.error().path == "animation");

  const auto reduced_motion =
      gisland::parse_theme(replace_once(std::string{valid_theme},
                                        "[animation.reduced_motion]\ncompact_to_expanded_ms = 0\n"
                                        "context_change_ms = 0\n\n",
                                        ""),
                           "reduced-motion.toml");
  REQUIRE_FALSE(reduced_motion.has_value());
  CHECK(reduced_motion.error().path == "animation.reduced_motion");
}
