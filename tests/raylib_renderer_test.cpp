#include "gisland/raylib_renderer.hpp"

#include "gisland/layout.hpp"
#include "gisland/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <raylib.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view theme_text = R"(
[palette]
surface = "#101820"
foreground = "#F0F0F0"
muted = "#405060"
accent = "#3050E0"
success = "#20C060"
warning = "#F0A020"
error = "#E03030"

[fonts]
ui = "fonts/Inter-Regular.ttf"
symbols = "fonts/Font-Awesome-6-Free-Solid-900.otf"

[typography.body]
font = "ui"
size = 24
line_height = 1

[typography.caption]
font = "ui"
size = 24
line_height = 1

[typography.title]
font = "ui"
size = 32
line_height = 1

[images.notification-icon]
width = 24
height = 24
fit = "cover"
shape = "circle"

[gaps]
normal = 4

[spacers]
normal = 6

[view.compact]
padding = 4
radius = 8
border = 1
min_width = 40
max_width = 100
min_height = 20
max_height = 60

[view.expanded]
padding = 8
radius = 12
border = 2
min_width = 80
max_width = 160
min_height = 60
max_height = 120

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
codepoint = 0xF133
)";

class HiddenWindow {
public:
  HiddenWindow() {
    SetTraceLogLevel(LOG_ERROR);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(128, 96, "gisland raylib renderer test");
    if (!IsWindowReady()) {
      throw std::runtime_error{"raylib test window failed to initialize"};
    }
  }

  HiddenWindow(const HiddenWindow &) = delete;
  HiddenWindow &operator=(const HiddenWindow &) = delete;

  ~HiddenWindow() {
    if (IsWindowReady()) {
      CloseWindow();
    }
  }
};

[[nodiscard]] gisland::Theme make_theme(std::string_view text = theme_text) {
  return gisland::parse_theme(text, "renderer-theme.toml").value();
}

[[nodiscard]] std::filesystem::path asset_root() { return GISLAND_TEST_ASSET_ROOT; }

[[nodiscard]] bool same_color(Color left, gisland::Rgba right) {
  return left.r == right.red && left.g == right.green && left.b == right.blue &&
         left.a == right.alpha;
}

template <typename Draw> [[nodiscard]] Image render_image(Draw &&draw) {
  RenderTexture2D target = LoadRenderTexture(128, 96);
  REQUIRE(IsRenderTextureValid(target));
  BeginTextureMode(target);
  ClearBackground(BLANK);
  std::forward<Draw>(draw)();
  EndTextureMode();
  Image image = LoadImageFromTexture(target.texture);
  ImageFlipVertical(&image);
  UnloadRenderTexture(target);
  return image;
}

[[nodiscard]] int colored_pixels(const Image &image, gisland::Rect area) {
  int count = 0;
  for (int y = area.y; y < area.y + area.height; ++y) {
    for (int x = area.x; x < area.x + area.width; ++x) {
      const Color pixel = GetImageColor(image, x, y);
      if (pixel.r != 0 || pixel.g != 0 || pixel.b != 0) {
        ++count;
      }
    }
  }
  return count;
}

} // namespace

TEST_CASE_METHOD(HiddenWindow, "font book loads pinned glyphs and measures UTF-8") {
  auto fonts = gisland::RaylibFontBook::load(make_theme(), asset_root());

  REQUIRE(fonts.has_value());
  CHECK(fonts->loaded_font_count() == 3);
  const auto body = make_theme().typography().at("body");
  const auto latin =
      fonts->measure_text("fonts/Inter-Regular.ttf", body, "Cafe \xC3\xA9 \xE2\x80\xA6");
  const auto icon =
      fonts->measure_codepoint("fonts/Font-Awesome-6-Free-Solid-900.otf", body, U'\uF133');
  CHECK(latin.width > 0.0);
  CHECK(latin.height >= body.size);
  CHECK(icon.width > 0.0);
  CHECK(icon.height >= body.size);
}

TEST_CASE_METHOD(HiddenWindow, "pinned Inter glyphs accept Cyrillic and reject unsupported text") {
  const auto theme = make_theme();
  auto fonts = gisland::RaylibFontBook::load(theme, asset_root());
  REQUIRE(fonts.has_value());

  const auto cyrillic = gisland::layout_scene(
      gisland::SceneNode{gisland::Text{"\u041f\u0440\u0438\u0432\u0435\u0442", "body"}}, theme,
      gisland::ViewMode::compact, *fonts);
  REQUIRE(cyrillic.has_value());

  const auto cjk = gisland::layout_scene(gisland::SceneNode{gisland::Text{"\u4f60\u597d", "body"}},
                                         theme, gisland::ViewMode::compact, *fonts);
  REQUIRE_FALSE(cjk.has_value());
  CHECK(cjk.error().code == gisland::LayoutErrorCode::unsupported_glyph);
  CHECK(cjk.error().path == "/value");

  const auto invalid =
      gisland::layout_scene(gisland::SceneNode{gisland::Text{std::string{"\xC3\x28", 2}, "body"}},
                            theme, gisland::ViewMode::compact, *fonts);
  REQUIRE_FALSE(invalid.has_value());
  CHECK(invalid.error().code == gisland::LayoutErrorCode::invalid_utf8);
  CHECK(invalid.error().path == "/value");
}

TEST_CASE("font book releases CPU allocations after its window closes") {
  std::optional<gisland::RaylibFontBook> fonts;
  {
    HiddenWindow window;
    auto loaded = gisland::RaylibFontBook::load(make_theme(), asset_root());
    REQUIRE(loaded.has_value());
    fonts.emplace(std::move(*loaded));
  }

  fonts.reset();
  SUCCEED();
}

TEST_CASE("closing and reopening a window does not release fonts from the new context") {
  std::optional<gisland::RaylibFontBook> old_fonts;
  {
    HiddenWindow window;
    auto loaded = gisland::RaylibFontBook::load(make_theme(), asset_root());
    REQUIRE(loaded.has_value());
    old_fonts.emplace(std::move(*loaded));
  }

  HiddenWindow replacement_window;
  auto replacement_fonts = gisland::RaylibFontBook::load(make_theme(), asset_root());
  REQUIRE(replacement_fonts.has_value());
  const gisland::RaylibPainter painter{*replacement_fonts};
  const auto body = make_theme().typography().at("body");
  const gisland::LayoutPlan plan{
      gisland::RoundedView{gisland::Rect{0, 0, 1, 1}, 0, 0, {}, {}},
      {gisland::TextDrawCommand{gisland::Rect{8, 8, 40, 24}, gisland::Rect{8, 8, 40, 24}, "Hi",
                                "fonts/Inter-Regular.ttf", body,
                                gisland::Rgba{240, 240, 240, 255}}}};

  old_fonts.reset();
  Image content = render_image([&] { REQUIRE(painter.draw_content(plan).has_value()); });
  CHECK(colored_pixels(content, gisland::Rect{8, 8, 40, 24}) > 0);
  UnloadImage(content);
}

TEST_CASE_METHOD(HiddenWindow, "font book reports missing resources without aborting") {
  std::string missing_theme{theme_text};
  const auto position = missing_theme.find("fonts/Inter-Regular.ttf");
  REQUIRE(position != std::string::npos);
  missing_theme.replace(position, std::string_view{"fonts/Inter-Regular.ttf"}.size(),
                        "fonts/missing.ttf");

  const auto fonts = gisland::RaylibFontBook::load(make_theme(missing_theme), asset_root());

  REQUIRE_FALSE(fonts.has_value());
  CHECK(fonts.error().code == gisland::RendererErrorCode::missing_resource);
  CHECK(fonts.error().resource.filename() == "missing.ttf");

  std::string directory_theme{theme_text};
  const auto directory_position = directory_theme.find("fonts/Inter-Regular.ttf");
  REQUIRE(directory_position != std::string::npos);
  directory_theme.replace(directory_position, std::string_view{"fonts/Inter-Regular.ttf"}.size(),
                          "fonts");
  const auto directory = gisland::RaylibFontBook::load(make_theme(directory_theme), asset_root());
  REQUIRE_FALSE(directory.has_value());
  CHECK(directory.error().code == gisland::RendererErrorCode::invalid_resource);

  std::string invalid_font_theme{theme_text};
  const auto invalid_position = invalid_font_theme.find("fonts/Inter-Regular.ttf");
  REQUIRE(invalid_position != std::string::npos);
  invalid_font_theme.replace(invalid_position, std::string_view{"fonts/Inter-Regular.ttf"}.size(),
                             "fonts/README.md");
  const auto invalid_font =
      gisland::RaylibFontBook::load(make_theme(invalid_font_theme), asset_root());
  REQUIRE_FALSE(invalid_font.has_value());
  CHECK(invalid_font.error().code == gisland::RendererErrorCode::font_load_failed);
}

TEST_CASE_METHOD(HiddenWindow, "painter keeps surface and ordered content operations separate") {
  auto fonts = gisland::RaylibFontBook::load(make_theme(), asset_root());
  REQUIRE(fonts.has_value());
  const gisland::RaylibPainter painter{*fonts};
  const auto body = make_theme().typography().at("body");

  gisland::LayoutPlan plan{
      gisland::RoundedView{gisland::Rect{4, 16, 80, 48}, 12, 2, gisland::Rgba{16, 24, 32, 255},
                           gisland::Rgba{32, 192, 96, 255}},
      {gisland::TextDrawCommand{gisland::Rect{30, 28, 40, 24}, gisland::Rect{30, 28, 40, 24}, "Hi",
                                "fonts/Inter-Regular.ttf", body, gisland::Rgba{240, 240, 240, 255}},
       gisland::IconDrawCommand{gisland::Rect{82, 28, 24, 24}, gisland::Rect{82, 28, 24, 24},
                                "fonts/Font-Awesome-6-Free-Solid-900.otf", body, U'\uF133',
                                gisland::Rgba{240, 160, 32, 255}, "Calendar"},
       gisland::ProgressDrawCommand{gisland::Rect{4, 36, 40, 8}, gisland::Rect{14, 36, 30, 8},
                                    gisland::Rect{4, 36, 40, 8}, gisland::Rect{4, 36, 20, 8},
                                    gisland::Rgba{64, 80, 96, 255},
                                    gisland::Rgba{224, 48, 48, 255}},
       gisland::ButtonDecorationDrawCommand{gisland::Rect{50, 32, 30, 16},
                                            gisland::Rect{50, 32, 30, 16},
                                            gisland::Rgba{224, 48, 48, 255}, true},
       gisland::ButtonDecorationDrawCommand{gisland::Rect{50, 32, 30, 16},
                                            gisland::Rect{50, 32, 30, 16},
                                            gisland::Rgba{48, 80, 224, 255}, true}}};

  gisland::LayoutPlan orientation{
      gisland::RoundedView{gisland::Rect{0, 0, 1, 1}, 0, 0, {}, {}},
      {gisland::ButtonDecorationDrawCommand{gisland::Rect{8, 4, 12, 8}, gisland::Rect{8, 4, 12, 8},
                                            gisland::Rgba{32, 192, 96, 255}, true}}};
  Image oriented = render_image([&] { REQUIRE(painter.draw_content(orientation).has_value()); });
  CHECK(same_color(GetImageColor(oriented, 12, 6), gisland::Rgba{32, 192, 96, 255}));
  const Color mirrored = GetImageColor(oriented, 12, 89);
  CHECK(mirrored.r == 0);
  CHECK(mirrored.g == 0);
  CHECK(mirrored.b == 0);
  UnloadImage(oriented);

  Image surface = render_image(
      [&] { REQUIRE(painter.draw_surface(plan, gisland::RenderOrigin{8, 8}).has_value()); });
  CHECK(same_color(GetImageColor(surface, 52, 48), plan.view.surface));
  const Color surface_corner = GetImageColor(surface, 8, 8);
  CHECK(surface_corner.r == 0);
  CHECK(surface_corner.g == 0);
  CHECK(surface_corner.b == 0);
  CHECK(colored_pixels(surface, gisland::Rect{8, 20, 88, 60}) > 0);
  UnloadImage(surface);

  Image content = render_image(
      [&] { REQUIRE(painter.draw_content(plan, gisland::RenderOrigin{8, 8}).has_value()); });
  const Color clipped_progress = GetImageColor(content, 16, 48);
  CHECK(clipped_progress.r == 0);
  CHECK(clipped_progress.g == 0);
  CHECK(clipped_progress.b == 0);
  CHECK(same_color(GetImageColor(content, 24, 48), gisland::Rgba{224, 48, 48, 255}));
  CHECK(same_color(GetImageColor(content, 38, 48), gisland::Rgba{64, 80, 96, 255}));
  CHECK(same_color(GetImageColor(content, 73, 50), gisland::Rgba{48, 80, 224, 255}));
  CHECK(colored_pixels(content, gisland::Rect{38, 36, 40, 24}) > 0);
  CHECK(colored_pixels(content, gisland::Rect{90, 36, 24, 24}) > 0);
  UnloadImage(content);
}

TEST_CASE_METHOD(HiddenWindow, "image book center-crops and masks dynamic images") {
  const auto theme = make_theme();
  auto fonts = gisland::RaylibFontBook::load(theme, asset_root());
  REQUIRE(fonts.has_value());
  const auto pixels = std::make_shared<const std::vector<std::uint8_t>>(
      std::vector<std::uint8_t>{255, 0, 0, 255, 0, 0, 255, 255});
  const std::vector resources{
      gisland::ImageResource{"icon", gisland::ImageFormat::rgba8, 2, 1, pixels},
      gisland::ImageResource{"same-icon", gisland::ImageFormat::rgba8, 2, 1, pixels}};
  auto images = gisland::RaylibImageBook::load(resources);
  REQUIRE(images.has_value());
  const gisland::LayoutPlan plan{
      gisland::RoundedView{gisland::Rect{0, 0, 32, 32}, 0, 0, {}, {}},
      {gisland::ImageDrawCommand{gisland::Rect{8, 8, 24, 24}, gisland::Rect{8, 8, 24, 24}, "icon",
                                 theme.images().at("notification-icon"), "Application"},
       gisland::ImageDrawCommand{gisland::Rect{40, 8, 24, 24}, gisland::Rect{40, 8, 24, 24},
                                 "same-icon", theme.images().at("notification-icon"),
                                 "Application"}}};
  REQUIRE(images->prepare(plan).has_value());
  CHECK(images->loaded_texture_count() == 1);
  const gisland::RaylibPainter painter{*fonts, *images};

  ::Image rendered = render_image([&] { REQUIRE(painter.draw_content(plan).has_value()); });

  CHECK(GetImageColor(rendered, 8, 8).a == 0);
  CHECK(GetImageColor(rendered, 31, 8).a == 0);
  const Color left = GetImageColor(rendered, 12, 20);
  const Color right = GetImageColor(rendered, 27, 20);
  CHECK(left.r > left.b);
  CHECK(right.b > right.r);
  UnloadImage(rendered);
}

TEST_CASE_METHOD(HiddenWindow, "painter reports a command font absent from the book") {
  auto fonts = gisland::RaylibFontBook::load(make_theme(), asset_root());
  REQUIRE(fonts.has_value());
  const gisland::RaylibPainter painter{*fonts};
  gisland::LayoutPlan plan{
      gisland::RoundedView{gisland::Rect{0, 0, 32, 32}, 4, 0, gisland::Rgba{0, 0, 0, 255}, {}},
      {gisland::TextDrawCommand{gisland::Rect{0, 0, 20, 20}, gisland::Rect{0, 0, 20, 20}, "x",
                                "fonts/not-loaded.ttf", make_theme().typography().at("body"),
                                gisland::Rgba{255, 255, 255, 255}}}};

  BeginDrawing();
  const auto drawn = painter.draw_content(plan);
  EndDrawing();

  REQUIRE_FALSE(drawn.has_value());
  CHECK(drawn.error().code == gisland::RendererErrorCode::font_not_loaded);
}

TEST_CASE_METHOD(HiddenWindow, "surface painter draws the resolved shadow before the opaque view") {
  auto fonts = gisland::RaylibFontBook::load(make_theme(), asset_root());
  REQUIRE(fonts.has_value());
  const gisland::RaylibPainter painter{*fonts};
  gisland::LayoutPlan plan{
      gisland::RoundedView{gisland::Rect{30, 24, 40, 32},
                           8,
                           0,
                           gisland::Rgba{16, 24, 32, 255},
                           {},
                           gisland::ViewShadow{8, 0, 0, 2, gisland::Rgba{224, 48, 48, 255}}},
      {}};

  Image image = render_image([&] { REQUIRE(painter.draw_surface(plan).has_value()); });

  CHECK(same_color(GetImageColor(image, 77, 40), gisland::Rgba{224, 48, 48, 255}));
  CHECK(same_color(GetImageColor(image, 50, 40), plan.view.surface));
  UnloadImage(image);
}

TEST_CASE_METHOD(HiddenWindow, "painter rejects coordinates that overflow after translation") {
  auto fonts = gisland::RaylibFontBook::load(make_theme(), asset_root());
  REQUIRE(fonts.has_value());
  const gisland::RaylibPainter painter{*fonts};
  const gisland::LayoutPlan plan{
      gisland::RoundedView{gisland::Rect{1, 0, 32, 32}, 4, 0, gisland::Rgba{16, 24, 32, 255}, {}},
      {}};

  const auto drawn =
      painter.draw_surface(plan, gisland::RenderOrigin{std::numeric_limits<int>::max(), 0});

  REQUIRE_FALSE(drawn.has_value());
  CHECK(drawn.error().code == gisland::RendererErrorCode::invalid_geometry);

  const gisland::LayoutPlan overflowing_clip{
      gisland::RoundedView{gisland::Rect{0, 0, 1, 1}, 0, 0, {}, {}},
      {gisland::ButtonDecorationDrawCommand{gisland::Rect{std::numeric_limits<int>::max(), 0, 1, 1},
                                            gisland::Rect{std::numeric_limits<int>::max(), 0, 1, 1},
                                            gisland::Rgba{255, 255, 255, 255}, true}}};
  const auto content = painter.draw_content(overflowing_clip);
  REQUIRE_FALSE(content.has_value());
  CHECK(content.error().code == gisland::RendererErrorCode::invalid_geometry);
}
