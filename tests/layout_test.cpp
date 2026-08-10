#include "gisland/layout.hpp"

#include "gisland/scene.hpp"
#include "gisland/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr std::string_view theme_text = R"(
[palette]
surface = "#000000"
foreground = "#F0F0F0"
muted = "#808080"
accent = "#8040FF"
success = "#20C060"
warning = "#F0A020"
error = "#E03030"

[fonts]
ui = "/fonts/ui.ttf"
symbols = "/fonts/symbols.ttf"

[typography.body]
font = "ui"
size = 10
line_height = 1

[typography.title]
font = "ui"
color = "muted"
size = 20
line_height = 1

[images.notification-icon]
width = 24
height = 24
fit = "cover"
shape = "circle"

[gaps]
normal = 4
small = 2

[spacers]
normal = 6
small = 2
huge = 100

[view.compact]
padding = 4
radius = 8
border = 1
min_width = 40
max_width = 100
min_height = 20
max_height = 50

[view.expanded]
padding = 10
radius = 12
border = 2
min_width = 100
max_width = 200
min_height = 60
max_height = 140

[shadow]
offset_x = 0
offset_y = 0
blur = 0
spread = 0
color = "#00000000"

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

class TestGlyphMetrics final : public gisland::GlyphMetrics {
public:
  [[nodiscard]] gisland::MeasuredGlyphs measure_text(std::string_view /*font_resource*/,
                                                     const gisland::TypographyRole &role,
                                                     std::string_view text) const override {
    std::size_t codepoints = 0;
    for (const unsigned char byte : text) {
      if ((byte & 0xC0U) != 0x80U) {
        ++codepoints;
      }
    }
    return {static_cast<double>(codepoints) * role.size, role.size * role.line_height};
  }

  [[nodiscard]] gisland::MeasuredGlyphs measure_codepoint(std::string_view /*font_resource*/,
                                                          const gisland::TypographyRole &role,
                                                          char32_t /*codepoint*/) const override {
    return {role.size, role.size * role.line_height};
  }
};

class TestRichTextMetrics final : public gisland::RichTextMetrics {
public:
  [[nodiscard]] std::expected<gisland::RichTextComposition, gisland::RichTextError>
  compose(const gisland::RichText &rich_text, int assigned_width) const override {
    gisland::RichTextComposition result{
        .assigned_width = assigned_width,
        .natural_width = 160,
        .minimum_width = 20,
        .height = assigned_width <= 80 ? 60 : 20,
        .line_count = assigned_width <= 80 ? 3 : 1,
        .unknown_glyphs = 0,
        .links = {},
        .images = {},
    };
    for (const auto &item : rich_text.content) {
      if (const auto *link = std::get_if<gisland::RichLinkSpan>(&item); link != nullptr) {
        result.links.push_back(
            {{0, 0, std::min(50, assigned_width), 20}, link->action_id, link->accessible_label});
        if (assigned_width <= 80) {
          result.links.push_back(
              {{0, 20, std::min(40, assigned_width), 20}, link->action_id, link->accessible_label});
        }
      }
    }
    return result;
  }
};

class AdversarialGlyphMetrics final : public gisland::GlyphMetrics {
public:
  explicit AdversarialGlyphMetrics(double width, double height = 10.0)
      : width_(width), height_(height) {}

  [[nodiscard]] gisland::MeasuredGlyphs measure_text(std::string_view /*font_resource*/,
                                                     const gisland::TypographyRole & /*role*/,
                                                     std::string_view /*text*/) const override {
    return {width_, height_};
  }

  [[nodiscard]] gisland::MeasuredGlyphs measure_codepoint(std::string_view /*font_resource*/,
                                                          const gisland::TypographyRole & /*role*/,
                                                          char32_t /*codepoint*/) const override {
    return {width_, height_};
  }

private:
  double width_;
  double height_;
};

class FractionalEllipsisMetrics final : public gisland::GlyphMetrics {
public:
  [[nodiscard]] gisland::MeasuredGlyphs measure_text(std::string_view /*font_resource*/,
                                                     const gisland::TypographyRole & /*role*/,
                                                     std::string_view text) const override {
    if (text == "\xE2\x80\xA6") {
      return {10.4, 10.0};
    }
    return {100.0, 10.0};
  }

  [[nodiscard]] gisland::MeasuredGlyphs measure_codepoint(std::string_view /*font_resource*/,
                                                          const gisland::TypographyRole & /*role*/,
                                                          char32_t /*codepoint*/) const override {
    return {10.0, 10.0};
  }
};

class RestrictedGlyphMetrics final : public gisland::GlyphMetrics {
public:
  [[nodiscard]] gisland::MeasuredGlyphs measure_text(std::string_view /*font_resource*/,
                                                     const gisland::TypographyRole &role,
                                                     std::string_view text) const override {
    return {static_cast<double>(text.size()) * role.size, role.size * role.line_height};
  }

  [[nodiscard]] gisland::MeasuredGlyphs measure_codepoint(std::string_view /*font_resource*/,
                                                          const gisland::TypographyRole &role,
                                                          char32_t /*codepoint*/) const override {
    return {role.size, role.size * role.line_height};
  }

  [[nodiscard]] bool supports_text(std::string_view /*font_resource*/,
                                   const gisland::TypographyRole & /*role*/,
                                   std::string_view text) const override {
    return text != "unsupported";
  }

  [[nodiscard]] bool supports_codepoint(std::string_view /*font_resource*/,
                                        const gisland::TypographyRole & /*role*/,
                                        char32_t codepoint) const override {
    return codepoint != U'\uE001';
  }
};

[[nodiscard]] gisland::Theme make_theme() {
  return gisland::parse_theme(theme_text, "layout-theme.toml").value();
}

[[nodiscard]] gisland::Theme make_theme_with(std::string_view before, std::string_view after) {
  std::string source{theme_text};
  const auto position = source.find(before);
  REQUIRE(position != std::string::npos);
  source.replace(position, before.size(), after);
  return gisland::parse_theme(source, "layout-theme.toml").value();
}

[[nodiscard]] gisland::Theme make_leading_cap_theme() {
  std::string source{theme_text};
  auto replace = [&source](std::string_view before, std::string_view after) {
    const auto position = source.find(before);
    REQUIRE(position != std::string::npos);
    source.replace(position, before.size(), after);
  };
  replace("shape = \"circle\"", "shape = \"circle\"\nplacement = \"leading-cap\"");
  replace("padding = 4\nradius = 8\nborder = 1\nmin_width = 40\nmax_width = 100\nmin_height "
          "= 20\nmax_height = 50",
          "padding_horizontal = 14\npadding_vertical = 4\nradius = 16\nborder = 0\nmin_width "
          "= 40\nmax_width = 76\nmin_height = 32\nmax_height = 32");
  return gisland::parse_theme(source, "leading-cap-theme.toml").value();
}

template <typename Command>
[[nodiscard]] const Command &command_at(const gisland::LayoutPlan &plan, std::size_t index) {
  REQUIRE(index < plan.content.size());
  return std::get<Command>(plan.content[index]);
}

[[nodiscard]] std::vector<gisland::SceneNode> texts(std::initializer_list<std::string_view> values,
                                                    std::string role = "body") {
  std::vector<gisland::SceneNode> result;
  result.reserve(values.size());
  for (const auto value : values) {
    result.emplace_back(gisland::Text{std::string{value}, role});
  }
  return result;
}

} // namespace

TEST_CASE("text layout resolves typography and compact view style") {
  const auto result =
      gisland::layout_scene(gisland::SceneNode{gisland::Text{"abc", "body"}}, make_theme(),
                            gisland::ViewMode::compact, TestGlyphMetrics{});

  REQUIRE(result.has_value());
  CHECK(result->view.bounds == gisland::Rect{0, 0, 40, 20});
  CHECK(result->view.radius == 8);
  CHECK(result->view.border == 1);
  CHECK(result->view.surface == gisland::Rgba{0, 0, 0, 255});
  REQUIRE(result->content.size() == 1);
  const auto &text = command_at<gisland::TextDrawCommand>(*result, 0);
  CHECK(text.text == "abc");
  CHECK(text.font_resource == "/fonts/ui.ttf");
  CHECK(text.bounds == gisland::Rect{4, 5, 30, 10});
  CHECK(text.clip == gisland::Rect{4, 4, 32, 12});
  CHECK(text.color == gisland::Rgba{240, 240, 240, 255});

  const auto one_line =
      gisland::layout_scene(gisland::SceneNode{gisland::Text{"a\r\nb\nc", "body"}}, make_theme(),
                            gisland::ViewMode::compact, TestGlyphMetrics{});
  REQUIRE(one_line.has_value());
  CHECK(command_at<gisland::TextDrawCommand>(*one_line, 0).text == "a b c");
}

TEST_CASE("view layout applies horizontal and vertical padding independently") {
  const auto theme = make_theme_with("padding = 4", "padding_horizontal = 7\npadding_vertical = 2");
  const auto result = gisland::layout_scene(gisland::SceneNode{gisland::Text{"abc", "body"}}, theme,
                                            gisland::ViewMode::compact, TestGlyphMetrics{});

  REQUIRE(result.has_value());
  CHECK(result->view.bounds == gisland::Rect{0, 0, 44, 20});
  REQUIRE(result->content.size() == 1);
  const auto &text = command_at<gisland::TextDrawCommand>(*result, 0);
  CHECK(text.bounds == gisland::Rect{7, 5, 30, 10});
  CHECK(text.clip == gisland::Rect{7, 2, 30, 16});
}

TEST_CASE("rich layout composes at assigned width and propagates wrapped height and links") {
  const gisland::RichText rich{
      .role = "body",
      .content = {gisland::RichTextSpan{"Wrapped ", {}},
                  gisland::RichLinkSpan{"link content", {}, "open-link", "Open link"}},
  };
  const gisland::SceneNode scene{gisland::ActionRegion{
      gisland::SceneNode{gisland::Column{
          {gisland::SceneNode{rich}, gisland::SceneNode{gisland::Text{"tail", "body"}}},
          "start",
          "normal"}},
      "default", true, "Open notification"}};
  const auto theme =
      make_theme_with("min_width = 100\nmax_width = 200", "min_width = 100\nmax_width = 100");

  const auto result = gisland::layout_scene(scene, theme, gisland::ViewMode::expanded,
                                            TestGlyphMetrics{}, TestRichTextMetrics{});

  REQUIRE(result.has_value());
  CHECK(result->view.bounds == gisland::Rect{0, 0, 100, 94});
  REQUIRE(result->content.size() == 2);
  const auto &command = command_at<gisland::RichTextDrawCommand>(*result, 0);
  CHECK(command.bounds == gisland::Rect{10, 10, 80, 60});
  CHECK(command.composition.assigned_width == 80);
  CHECK(command.composition.line_count == 3);
  REQUIRE(result->interactions.size() == 3);
  CHECK(result->interactions[0] ==
        gisland::InteractionTarget{{10, 10, 80, 74},
                                   {10, 10, 80, 74},
                                   "default",
                                   true,
                                   "Open notification",
                                   gisland::InteractionKind::action_region});
  CHECK(result->interactions[1] == gisland::InteractionTarget{{10, 10, 50, 20},
                                                              {10, 10, 50, 20},
                                                              "open-link",
                                                              true,
                                                              "Open link",
                                                              gisland::InteractionKind::link});
  CHECK(result->interactions[2] == gisland::InteractionTarget{{10, 30, 40, 20},
                                                              {10, 30, 40, 20},
                                                              "open-link",
                                                              true,
                                                              "Open link",
                                                              gisland::InteractionKind::link});
}

TEST_CASE("rich layout receives remaining row width and never uses plain glyph metrics") {
  const gisland::RichText rich{
      .role = "body",
      .content = {gisland::RichLinkSpan{"link content", {}, "open-link", "Open link"}},
  };
  const auto theme =
      make_theme_with("min_width = 100\nmax_width = 200", "min_width = 100\nmax_width = 100");
  const gisland::SceneNode scene{
      gisland::Row{{gisland::SceneNode{rich}, gisland::SceneNode{gisland::Text{"tail", "body"}}},
                   "center",
                   "normal"}};

  const auto missing =
      gisland::layout_scene(scene, theme, gisland::ViewMode::expanded, TestGlyphMetrics{});
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().path == "/children/0");

  const auto result = gisland::layout_scene(scene, theme, gisland::ViewMode::expanded,
                                            TestGlyphMetrics{}, TestRichTextMetrics{});
  REQUIRE(result.has_value());
  CHECK(result->view.bounds == gisland::Rect{0, 0, 100, 80});
  CHECK(command_at<gisland::RichTextDrawCommand>(*result, 0).bounds ==
        gisland::Rect{10, 10, 36, 60});
  CHECK(command_at<gisland::TextDrawCommand>(*result, 1).bounds == gisland::Rect{50, 35, 40, 10});
}

TEST_CASE("icon layout resolves the semantic glyph without raylib types") {
  const auto result =
      gisland::layout_scene(gisland::SceneNode{gisland::Icon{"calendar", "Calendar"}}, make_theme(),
                            gisland::ViewMode::compact, TestGlyphMetrics{});

  REQUIRE(result.has_value());
  const auto &icon = command_at<gisland::IconDrawCommand>(*result, 0);
  CHECK(icon.codepoint == U'\uE001');
  CHECK(icon.font_resource == "/fonts/symbols.ttf");
  CHECK(icon.typography.size == 10.0);
  CHECK(icon.bounds == gisland::Rect{4, 5, 10, 10});
  CHECK(icon.accessible_label == "Calendar");
}

TEST_CASE("image layout resolves semantic geometry without intrinsic pixel dimensions") {
  const auto result = gisland::layout_scene(
      gisland::SceneNode{gisland::Image{"app-icon", "notification-icon", "Firefox"}}, make_theme(),
      gisland::ViewMode::compact, TestGlyphMetrics{});

  REQUIRE(result.has_value());
  REQUIRE(result->content.size() == 1);
  const auto &image = command_at<gisland::ImageDrawCommand>(*result, 0);
  CHECK(image.resource_id == "app-icon");
  CHECK(image.bounds == gisland::Rect{4, 4, 24, 24});
  CHECK(image.clip == gisland::Rect{4, 4, 32, 24});
  CHECK(image.style.fit == gisland::ImageFit::cover);
  CHECK(image.style.shape == gisland::ImageShape::circle);
  CHECK(image.accessible_label == "Firefox");
}

TEST_CASE("image layout rejects unknown semantic roles with an exact path") {
  const auto result = gisland::layout_scene(
      gisland::SceneNode{gisland::Image{"app-icon", "missing", "Application"}}, make_theme(),
      gisland::ViewMode::compact, TestGlyphMetrics{});

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == gisland::LayoutErrorCode::unknown_image_role);
  CHECK(result.error().path == "/role");
}

TEST_CASE("compact leading-cap image is concentric and leaves the remaining width to text") {
  const gisland::SceneNode scene{
      gisland::Row{{gisland::SceneNode{gisland::Image{"app-icon", "notification-icon", "Firefox"}},
                    gisland::SceneNode{gisland::Text{"abcdefgh", "body", "end"}}},
                   "center",
                   "normal"}};

  const auto result = gisland::layout_scene(scene, make_leading_cap_theme(),
                                            gisland::ViewMode::compact, TestGlyphMetrics{});

  REQUIRE(result.has_value());
  CHECK(result->view.bounds == gisland::Rect{0, 0, 76, 32});
  REQUIRE(result->content.size() == 2);
  const auto &image = command_at<gisland::ImageDrawCommand>(*result, 0);
  CHECK(image.bounds == gisland::Rect{4, 4, 24, 24});
  CHECK(image.clip == result->view.bounds);
  const auto &text = command_at<gisland::TextDrawCommand>(*result, 1);
  CHECK(text.bounds == gisland::Rect{32, 11, 30, 10});
  CHECK(text.text == "ab\xE2\x80\xA6");
}

TEST_CASE("leading-cap placement rejects non-root and non-compact use") {
  const auto theme = make_leading_cap_theme();
  const gisland::SceneNode nested{gisland::Column{{gisland::SceneNode{gisland::Row{
      {gisland::SceneNode{gisland::Image{"app-icon", "notification-icon", "Firefox"}}}}}}}};
  const auto wrong_location =
      gisland::layout_scene(nested, theme, gisland::ViewMode::compact, TestGlyphMetrics{});
  REQUIRE_FALSE(wrong_location.has_value());
  CHECK(wrong_location.error().path == "/children/0/children/0/role");

  const gisland::SceneNode root{gisland::Row{
      {gisland::SceneNode{gisland::Image{"app-icon", "notification-icon", "Firefox"}}}}};
  const auto expanded =
      gisland::layout_scene(root, theme, gisland::ViewMode::expanded, TestGlyphMetrics{});
  REQUIRE_FALSE(expanded.has_value());
  CHECK(expanded.error().path == "/children/0/role");
}

TEST_CASE("rows and columns preserve painter order and apply cross-axis alignment") {
  for (const auto &[alignment, expected_y] :
       std::vector<std::pair<std::string, int>>{{"start", 4}, {"center", 9}, {"end", 14}}) {
    const gisland::SceneNode row{gisland::Row{{gisland::SceneNode{gisland::Text{"a", "body"}},
                                               gisland::SceneNode{gisland::Text{"b", "title"}}},
                                              alignment}};
    const auto result =
        gisland::layout_scene(row, make_theme(), gisland::ViewMode::compact, TestGlyphMetrics{});

    REQUIRE(result.has_value());
    REQUIRE(result->content.size() == 2);
    CHECK(command_at<gisland::TextDrawCommand>(*result, 0).text == "a");
    CHECK(command_at<gisland::TextDrawCommand>(*result, 0).bounds.y == expected_y);
    CHECK(command_at<gisland::TextDrawCommand>(*result, 1).text == "b");
    CHECK(command_at<gisland::TextDrawCommand>(*result, 1).color ==
          gisland::Rgba{128, 128, 128, 255});
  }

  const gisland::SceneNode nested{gisland::Column{
      {gisland::SceneNode{gisland::Row{texts({"one", "two"}, "body"), "center", "small"}},
       gisland::SceneNode{gisland::Text{"three", "body"}}},
      "end",
      "small"}};
  const auto result =
      gisland::layout_scene(nested, make_theme(), gisland::ViewMode::expanded, TestGlyphMetrics{});

  REQUIRE(result.has_value());
  REQUIRE(result->content.size() == 3);
  CHECK(command_at<gisland::TextDrawCommand>(*result, 0).text == "one");
  CHECK(command_at<gisland::TextDrawCommand>(*result, 1).text == "two");
  CHECK(command_at<gisland::TextDrawCommand>(*result, 2).text == "three");
  CHECK(command_at<gisland::TextDrawCommand>(*result, 2).bounds.x == 40);
}

TEST_CASE("fixed and flexible spacers share constrained main-axis space") {
  const gisland::SceneNode scene{gisland::Row{{gisland::SceneNode{gisland::Text{"A", "body"}},
                                               gisland::SceneNode{gisland::Spacer{true, ""}},
                                               gisland::SceneNode{gisland::Spacer{true, ""}},
                                               gisland::SceneNode{gisland::Text{"B", "body"}}}}};
  const auto result =
      gisland::layout_scene(scene, make_theme(), gisland::ViewMode::expanded, TestGlyphMetrics{});

  REQUIRE(result.has_value());
  REQUIRE(result->content.size() == 2);
  CHECK(command_at<gisland::TextDrawCommand>(*result, 0).bounds.x == 10);
  CHECK(command_at<gisland::TextDrawCommand>(*result, 1).bounds.x == 80);

  const gisland::SceneNode fixed{gisland::Row{{gisland::SceneNode{gisland::Text{"A", "body"}},
                                               gisland::SceneNode{gisland::Spacer{false, "normal"}},
                                               gisland::SceneNode{gisland::Text{"B", "body"}}},
                                              "center",
                                              "small"}};
  const auto fixed_result =
      gisland::layout_scene(fixed, make_theme(), gisland::ViewMode::compact, TestGlyphMetrics{});
  REQUIRE(fixed_result.has_value());
  CHECK(command_at<gisland::TextDrawCommand>(*fixed_result, 1).bounds.x == 24);

  const gisland::SceneNode vertical{gisland::Column{
      {gisland::SceneNode{gisland::Text{"A", "body"}},
       gisland::SceneNode{gisland::Spacer{true, ""}}, gisland::SceneNode{gisland::Spacer{true, ""}},
       gisland::SceneNode{gisland::Text{"B", "body"}}}}};
  const auto vertical_result = gisland::layout_scene(
      vertical, make_theme(), gisland::ViewMode::expanded, TestGlyphMetrics{});
  REQUIRE(vertical_result.has_value());
  CHECK(command_at<gisland::TextDrawCommand>(*vertical_result, 0).bounds.y == 10);
  CHECK(command_at<gisland::TextDrawCommand>(*vertical_result, 1).bounds.y == 40);
}

TEST_CASE("same-axis expansion propagates through multiple nested containers") {
  const gisland::SceneNode horizontal{gisland::Row{
      {gisland::SceneNode{gisland::Row{
           {gisland::SceneNode{gisland::Row{{gisland::SceneNode{gisland::Text{"A", "body"}},
                                             gisland::SceneNode{gisland::Spacer{true, ""}},
                                             gisland::SceneNode{gisland::Text{"B", "body"}}},
                                            "center",
                                            "small"}}},
           "center",
           "small"}},
       gisland::SceneNode{gisland::Text{"C", "body"}}},
      "center",
      "small"}};
  const auto horizontal_result = gisland::layout_scene(
      horizontal, make_theme(), gisland::ViewMode::expanded, TestGlyphMetrics{});

  REQUIRE(horizontal_result.has_value());
  CHECK(command_at<gisland::TextDrawCommand>(*horizontal_result, 0).bounds.x == 10);
  CHECK(command_at<gisland::TextDrawCommand>(*horizontal_result, 1).bounds.x == 68);
  CHECK(command_at<gisland::TextDrawCommand>(*horizontal_result, 2).bounds.x == 80);

  const gisland::SceneNode vertical{gisland::Column{
      {gisland::SceneNode{gisland::Column{
           {gisland::SceneNode{gisland::Column{{gisland::SceneNode{gisland::Text{"A", "body"}},
                                                gisland::SceneNode{gisland::Spacer{true, ""}},
                                                gisland::SceneNode{gisland::Text{"B", "body"}}},
                                               "center",
                                               "small"}}},
           "center",
           "small"}},
       gisland::SceneNode{gisland::Text{"C", "body"}}},
      "center",
      "small"}};
  const auto vertical_result = gisland::layout_scene(
      vertical, make_theme(), gisland::ViewMode::expanded, TestGlyphMetrics{});

  REQUIRE(vertical_result.has_value());
  CHECK(command_at<gisland::TextDrawCommand>(*vertical_result, 0).bounds.y == 10);
  CHECK(command_at<gisland::TextDrawCommand>(*vertical_result, 1).bounds.y == 28);
  CHECK(command_at<gisland::TextDrawCommand>(*vertical_result, 2).bounds.y == 40);
}

TEST_CASE("expansion propagates through containers on alternating axes") {
  const gisland::SceneNode horizontal_flex{
      gisland::Row{{gisland::SceneNode{gisland::Text{"A", "body"}},
                    gisland::SceneNode{gisland::Spacer{true, ""}},
                    gisland::SceneNode{gisland::Text{"B", "body"}}},
                   "center",
                   "small"}};
  const gisland::SceneNode horizontal_nested{
      gisland::Column{{gisland::SceneNode{gisland::Column{{horizontal_flex}}}}}};
  const gisland::SceneNode horizontal{gisland::Row{
      {horizontal_nested, gisland::SceneNode{gisland::Text{"C", "body"}}}, "center", "small"}};
  const auto horizontal_result = gisland::layout_scene(
      horizontal, make_theme(), gisland::ViewMode::expanded, TestGlyphMetrics{});

  REQUIRE(horizontal_result.has_value());
  CHECK(command_at<gisland::TextDrawCommand>(*horizontal_result, 0).bounds.x == 10);
  CHECK(command_at<gisland::TextDrawCommand>(*horizontal_result, 1).bounds.x == 68);
  CHECK(command_at<gisland::TextDrawCommand>(*horizontal_result, 2).bounds.x == 80);

  const gisland::SceneNode vertical_flex{
      gisland::Column{{gisland::SceneNode{gisland::Text{"A", "body"}},
                       gisland::SceneNode{gisland::Spacer{true, ""}},
                       gisland::SceneNode{gisland::Text{"B", "body"}}},
                      "center",
                      "small"}};
  const gisland::SceneNode vertical_nested{
      gisland::Row{{gisland::SceneNode{gisland::Row{{vertical_flex}}}}}};
  const gisland::SceneNode vertical{gisland::Column{
      {vertical_nested, gisland::SceneNode{gisland::Text{"C", "body"}}}, "center", "small"}};
  const auto vertical_result = gisland::layout_scene(
      vertical, make_theme(), gisland::ViewMode::expanded, TestGlyphMetrics{});

  REQUIRE(vertical_result.has_value());
  CHECK(command_at<gisland::TextDrawCommand>(*vertical_result, 0).bounds.y == 10);
  CHECK(command_at<gisland::TextDrawCommand>(*vertical_result, 1).bounds.y == 28);
  CHECK(command_at<gisland::TextDrawCommand>(*vertical_result, 2).bounds.y == 40);
}

TEST_CASE("progress emits a measured label and rounded track fill") {
  const auto result =
      gisland::layout_scene(gisland::SceneNode{gisland::Progress{0.25, "Work", "success"}},
                            make_theme(), gisland::ViewMode::compact, TestGlyphMetrics{});

  REQUIRE(result.has_value());
  REQUIRE(result->content.size() == 2);
  const auto &label = command_at<gisland::TextDrawCommand>(*result, 0);
  const auto &progress = command_at<gisland::ProgressDrawCommand>(*result, 1);
  CHECK(label.text == "Work");
  CHECK(progress.track.width == 48);
  CHECK(progress.fill.width == 12);
  CHECK(progress.fill_color == gisland::Rgba{32, 192, 96, 255});
  CHECK(progress.track.y > label.bounds.y);

  const auto semantic_label = gisland::layout_scene(
      gisland::SceneNode{gisland::Progress{0.25, "Work", "success"}},
      make_theme_with("[typography.body]\nfont = \"ui\"",
                      "[typography.body]\nfont = \"ui\"\ncolor = \"warning\""),
      gisland::ViewMode::compact, TestGlyphMetrics{});
  REQUIRE(semantic_label.has_value());
  CHECK(command_at<gisland::TextDrawCommand>(*semantic_label, 0).color ==
        gisland::Rgba{240, 160, 32, 255});

  const auto unlabeled =
      gisland::layout_scene(gisland::SceneNode{gisland::Progress{0.5, "", "accent"}}, make_theme(),
                            gisland::ViewMode::compact, TestGlyphMetrics{});
  REQUIRE(unlabeled.has_value());
  REQUIRE(unlabeled->content.size() == 1);
  CHECK(std::holds_alternative<gisland::ProgressDrawCommand>(unlabeled->content.front()));
}

TEST_CASE("ring progress creates a 48 pixel intrinsic compact capsule") {
  const auto theme =
      make_theme_with("padding = 4\nradius = 8\nborder = 1\nmin_width = 40\nmax_width = "
                      "100\nmin_height = 20\nmax_height = 50",
                      "padding_horizontal = 4\npadding_vertical = 4\nradius = 16\nborder = "
                      "1\nmin_width = 48\nmax_width = 100\nmin_height = 32\nmax_height = 48");
  const auto result = gisland::layout_scene(
      gisland::SceneNode{gisland::Progress{0.72, "", "success", gisland::ProgressShape::ring}},
      theme, gisland::ViewMode::compact, TestGlyphMetrics{});

  REQUIRE(result.has_value());
  CHECK(result->view.bounds.height == 48);
  CHECK(result->view.radius == 24);
  REQUIRE(result->content.size() == 1);
  const auto &ring = command_at<gisland::RingProgressDrawCommand>(*result, 0);
  CHECK(ring.bounds == gisland::Rect{4, 8, 32, 32});
  CHECK(ring.value == 0.72);
  CHECK(ring.thickness == 4);
  CHECK(ring.fill_color == gisland::Rgba{32, 192, 96, 255});
  CHECK(ring.track_color == gisland::Rgba{32, 192, 96, 64});

  const auto warning = gisland::layout_scene(
      gisland::SceneNode{gisland::Progress{0.25, "", "warning", gisland::ProgressShape::ring}},
      theme, gisland::ViewMode::compact, TestGlyphMetrics{});
  REQUIRE(warning.has_value());
  CHECK(command_at<gisland::RingProgressDrawCommand>(*warning, 0).track_color ==
        gisland::Rgba{240, 160, 32, 64});
}

TEST_CASE("buttons emit disabled decoration before centered child content") {
  const auto result = gisland::layout_scene(
      gisland::SceneNode{
          gisland::Button{gisland::SceneNode{gisland::Text{"Go", "body"}}, "go", false, "Go now"}},
      make_theme(), gisland::ViewMode::expanded, TestGlyphMetrics{});

  REQUIRE(result.has_value());
  REQUIRE(result->content.size() == 2);
  REQUIRE(result->interactions.size() == 1);
  const auto &button = command_at<gisland::ButtonDecorationDrawCommand>(*result, 0);
  const auto &text = command_at<gisland::TextDrawCommand>(*result, 1);
  const auto &interaction = result->interactions.front();
  CHECK_FALSE(button.enabled);
  CHECK(button.color == gisland::Rgba{128, 128, 128, 255});
  CHECK(interaction.bounds == button.bounds);
  CHECK(interaction.clip == button.clip);
  CHECK(interaction.action_id == "go");
  CHECK_FALSE(interaction.enabled);
  CHECK(interaction.accessible_label == "Go now");
  CHECK(interaction.kind == gisland::InteractionKind::button);
  CHECK(text.bounds.x == button.bounds.x + ((button.bounds.width - text.bounds.width) / 2));
  CHECK(text.bounds.y == button.bounds.y + ((button.bounds.height - text.bounds.height) / 2));
}

TEST_CASE("buttons resolve configured backgrounds independently from the accent") {
  const auto theme = make_theme_with("[shadow]", "[buttons]\nbackground = \"surface\"\n"
                                                 "disabled_background = \"#102030\"\n\n[shadow]");
  const auto enabled =
      gisland::layout_scene(gisland::SceneNode{gisland::Button{
                                gisland::SceneNode{gisland::Text{"Go", "body"}}, "go", true, "Go"}},
                            theme, gisland::ViewMode::expanded, TestGlyphMetrics{});
  const auto disabled = gisland::layout_scene(
      gisland::SceneNode{gisland::Button{gisland::SceneNode{gisland::Text{"Stop", "body"}}, "stop",
                                         false, "Stop"}},
      theme, gisland::ViewMode::expanded, TestGlyphMetrics{});

  REQUIRE(enabled.has_value());
  REQUIRE(disabled.has_value());
  CHECK(command_at<gisland::ButtonDecorationDrawCommand>(*enabled, 0).color ==
        gisland::Rgba{0, 0, 0, 255});
  CHECK(command_at<gisland::ButtonDecorationDrawCommand>(*disabled, 0).color ==
        gisland::Rgba{16, 32, 48, 255});
}

TEST_CASE("shadow insets cover every configured overflow edge") {
  const gisland::RoundedView view{
      {0, 0, 100, 40}, 20, 0, {0, 0, 0, 255}, {}, gisland::ViewShadow{4, 6, 18, 2, {0, 0, 0, 102}},
  };

  CHECK(gisland::shadow_insets(view) == gisland::RectInsets{16, 14, 24, 26});

  auto transparent = view;
  transparent.shadow.color.alpha = 0;
  CHECK(gisland::shadow_insets(transparent) == gisland::RectInsets{});
}

TEST_CASE("text applies UTF-8-safe end truncation or painter clipping at the view maximum") {
  const auto end =
      gisland::layout_scene(gisland::SceneNode{gisland::Text{"abcdefghijklmno", "body", "end"}},
                            make_theme(), gisland::ViewMode::compact, TestGlyphMetrics{});
  REQUIRE(end.has_value());
  CHECK(end->view.bounds.width == 100);
  CHECK(command_at<gisland::TextDrawCommand>(*end, 0).text == "abcdefgh\u2026");
  CHECK(command_at<gisland::TextDrawCommand>(*end, 0).bounds.width == 92);

  const auto utf8 = gisland::layout_scene(
      gisland::SceneNode{gisland::Text{
          "\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9", "body", "end"}},
      make_theme(), gisland::ViewMode::compact, TestGlyphMetrics{});
  REQUIRE(utf8.has_value());
  CHECK(command_at<gisland::TextDrawCommand>(*utf8, 0).text ==
        "\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u2026");

  const auto clip =
      gisland::layout_scene(gisland::SceneNode{gisland::Text{"abcdefghijklmno", "body", "clip"}},
                            make_theme(), gisland::ViewMode::compact, TestGlyphMetrics{});
  REQUIRE(clip.has_value());
  CHECK(command_at<gisland::TextDrawCommand>(*clip, 0).text == "abcdefghijklmno");
  CHECK(command_at<gisland::TextDrawCommand>(*clip, 0).clip.width == 92);
}

TEST_CASE("ellipsis minimum and truncation use identical rounded pixel semantics") {
  const auto result = gisland::layout_scene(
      gisland::SceneNode{gisland::Text{"abcdef", "body", "end"}},
      make_theme_with("min_width = 40\nmax_width = 100", "min_width = 18\nmax_width = 18"),
      gisland::ViewMode::compact, FractionalEllipsisMetrics{});

  REQUIRE(result.has_value());
  CHECK(command_at<gisland::TextDrawCommand>(*result, 0).bounds.width == 10);
  CHECK(command_at<gisland::TextDrawCommand>(*result, 0).text == "\xE2\x80\xA6");
}

TEST_CASE("layout rejects invalid scenes at its entry boundary") {
  for (const double value :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
    const auto result =
        gisland::layout_scene(gisland::SceneNode{gisland::Progress{value, "", "accent"}},
                              make_theme(), gisland::ViewMode::compact, TestGlyphMetrics{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LayoutErrorCode::impossible_constraints);
    CHECK(result.error().path == "/value");
  }
}

TEST_CASE("layout rejects malformed UTF-8 and unsupported glyphs with field paths") {
  const auto malformed =
      gisland::layout_scene(gisland::SceneNode{gisland::Text{std::string{"\xC3\x28", 2}, "body"}},
                            make_theme(), gisland::ViewMode::compact, TestGlyphMetrics{});
  REQUIRE_FALSE(malformed.has_value());
  CHECK(malformed.error().code == gisland::LayoutErrorCode::invalid_utf8);
  CHECK(malformed.error().path == "/value");

  const auto text =
      gisland::layout_scene(gisland::SceneNode{gisland::Text{"unsupported", "body"}}, make_theme(),
                            gisland::ViewMode::compact, RestrictedGlyphMetrics{});
  REQUIRE_FALSE(text.has_value());
  CHECK(text.error().code == gisland::LayoutErrorCode::unsupported_glyph);
  CHECK(text.error().path == "/value");

  const auto icon =
      gisland::layout_scene(gisland::SceneNode{gisland::Icon{"calendar", "Calendar"}}, make_theme(),
                            gisland::ViewMode::compact, RestrictedGlyphMetrics{});
  REQUIRE_FALSE(icon.has_value());
  CHECK(icon.error().code == gisland::LayoutErrorCode::unsupported_glyph);
  CHECK(icon.error().path == "/name");
}

TEST_CASE("layout rejects post-rounding non-positive content bounds") {
  const auto result = gisland::layout_scene(
      gisland::SceneNode{gisland::Spacer{true, ""}},
      make_theme_with("padding = 4\nradius = 8\nborder = 1\nmin_width = 40\nmax_width = 100",
                      "padding = 4.6\nradius = 8\nborder = 1\nmin_width = 9.3\nmax_width = 9.3"),
      gisland::ViewMode::compact, TestGlyphMetrics{});

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == gisland::LayoutErrorCode::impossible_constraints);
}

TEST_CASE("adversarial glyph measurements cannot overflow layout arithmetic") {
  constexpr auto maximum = static_cast<double>(std::numeric_limits<int>::max());
  const auto check = [](gisland::SceneNode scene, const gisland::GlyphMetrics &metrics) {
    const auto result =
        gisland::layout_scene(scene, make_theme(), gisland::ViewMode::compact, metrics);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LayoutErrorCode::impossible_constraints);
  };

  check(gisland::SceneNode{gisland::Row{texts({"A", "B"})}}, AdversarialGlyphMetrics{maximum});
  check(gisland::SceneNode{gisland::Button{gisland::SceneNode{gisland::Text{"A", "body"}}, "open"}},
        AdversarialGlyphMetrics{maximum});
  check(gisland::SceneNode{gisland::Progress{0.5, "A", "accent"}},
        AdversarialGlyphMetrics{10.0, maximum});
}

TEST_CASE("layout returns structured paths for every unknown semantic token") {
  const auto check = [](gisland::SceneNode scene, gisland::LayoutErrorCode code,
                        std::string_view path) {
    const auto result =
        gisland::layout_scene(scene, make_theme(), gisland::ViewMode::compact, TestGlyphMetrics{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == code);
    CHECK(result.error().path == path);
  };

  check(gisland::SceneNode{gisland::Text{"x", "missing"}}, gisland::LayoutErrorCode::unknown_role,
        "/role");
  check(gisland::SceneNode{gisland::Icon{"missing", "Missing"}},
        gisland::LayoutErrorCode::unknown_icon, "/name");
  check(gisland::SceneNode{gisland::Row{texts({"x"}), "center", "missing"}},
        gisland::LayoutErrorCode::unknown_gap, "/gap");
  check(gisland::SceneNode{gisland::Spacer{false, "missing"}},
        gisland::LayoutErrorCode::unknown_spacer, "/size_token");
  check(gisland::SceneNode{gisland::Row{texts({"x"}), "stretch", "normal"}},
        gisland::LayoutErrorCode::unknown_alignment, "/alignment");
  check(gisland::SceneNode{gisland::Text{"x", "body", "middle"}},
        gisland::LayoutErrorCode::unknown_truncation, "/truncation");
  check(gisland::SceneNode{gisland::Progress{0.5, "", "missing"}},
        gisland::LayoutErrorCode::unknown_role, "/state");
}

TEST_CASE("compact and expanded modes use independent rounded view geometry") {
  const gisland::SceneNode scene{gisland::Text{"x", "body"}};
  const auto compact =
      gisland::layout_scene(scene, make_theme(), gisland::ViewMode::compact, TestGlyphMetrics{});
  const auto expanded =
      gisland::layout_scene(scene, make_theme(), gisland::ViewMode::expanded, TestGlyphMetrics{});

  REQUIRE(compact.has_value());
  REQUIRE(expanded.has_value());
  CHECK(compact->view.bounds == gisland::Rect{0, 0, 40, 20});
  CHECK(expanded->view.bounds == gisland::Rect{0, 0, 100, 60});
  CHECK(expanded->view.radius == 12);
  CHECK(expanded->view.border == 2);
  CHECK(command_at<gisland::TextDrawCommand>(*expanded, 0).bounds == gisland::Rect{10, 25, 10, 10});
}

TEST_CASE("rigid content that cannot fit the view returns an impossible constraint path") {
  const gisland::SceneNode scene{
      gisland::Row{{gisland::SceneNode{gisland::Icon{"calendar", "Calendar"}},
                    gisland::SceneNode{gisland::Spacer{false, "huge"}}}}};
  const auto result =
      gisland::layout_scene(scene, make_theme(), gisland::ViewMode::compact, TestGlyphMetrics{});

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == gisland::LayoutErrorCode::impossible_constraints);
  CHECK(result.error().path == "");
}

TEST_CASE("calendar uses stable text columns and keeps only header actions as buttons") {
  std::vector<gisland::SceneNode> weeks;
  weeks.emplace_back(
      gisland::Row{texts({"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"}), "center", "small"});
  weeks.emplace_back(
      gisland::Row{texts({"01", "02", "03", "04", "05", "06", "07"}), "center", "small"});
  auto current_week = texts({"08", "09", "10", "11", "12", "13", "14"});
  current_week[2] = gisland::SceneNode{gisland::Text{"10", "today"}};
  weeks.emplace_back(gisland::Row{std::move(current_week), "center", "small"});

  const gisland::SceneNode calendar{gisland::Column{
      {gisland::SceneNode{gisland::Row{
           {gisland::SceneNode{
                gisland::Button{gisland::SceneNode{gisland::Text{"<", "body"}}, "previous"}},
            gisland::SceneNode{gisland::Spacer{true, ""}},
            gisland::SceneNode{gisland::Text{"July", "body"}},
            gisland::SceneNode{gisland::Spacer{true, ""}},
            gisland::SceneNode{
                gisland::Button{gisland::SceneNode{gisland::Text{"Today", "body"}}, "today"}},
            gisland::SceneNode{
                gisland::Button{gisland::SceneNode{gisland::Text{">", "body"}}, "next"}}},
           "center",
           "small"}},
       gisland::SceneNode{gisland::Spacer{false, "small"}},
       gisland::SceneNode{gisland::Column{std::move(weeks), "center", "small"}}},
      "center",
      "small"}};

  const auto result = gisland::layout_scene(
      calendar,
      make_theme_with("[typography.title]",
                      "[typography.today]\nfont = \"ui\"\ncolor = \"accent\"\nsize = "
                      "10\nline_height = 1\n\n[typography.title]"),
      gisland::ViewMode::expanded, TestGlyphMetrics{});

  REQUIRE(result.has_value());
  CHECK(std::count_if(result->content.begin(), result->content.end(), [](const auto &command) {
          return std::holds_alternative<gisland::ButtonDecorationDrawCommand>(command);
        }) == 3);

  std::vector<const gisland::TextDrawCommand *> cells;
  for (const auto &command : result->content) {
    if (const auto *text = std::get_if<gisland::TextDrawCommand>(&command);
        text != nullptr && text->text.size() == 2) {
      cells.push_back(text);
    }
  }
  REQUIRE(cells.size() == 21);
  for (std::size_t row = 1; row < 3; ++row) {
    for (std::size_t column = 0; column < 7; ++column) {
      CHECK(cells[(row * 7) + column]->bounds.x == cells[column]->bounds.x);
    }
  }
  CHECK(cells[16]->text == "10");
  CHECK(cells[16]->color == gisland::Rgba{128, 64, 255, 255});
}
