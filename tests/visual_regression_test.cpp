#include "audio/audio_process.hpp"
#include "gisland/island.hpp"
#include "gisland/layout.hpp"
#include "gisland/raylib_renderer.hpp"
#include "gisland/scene.hpp"
#include "gisland/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <raylib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr int image_width = 480;
constexpr int image_height = 360;
constexpr int maximum_channel_delta = 3;
constexpr int maximum_divergent_pixels = 8;

class HiddenWindow {
public:
  HiddenWindow() {
    SetTraceLogLevel(LOG_ERROR);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(image_width, image_height, "gisland visual regression test");
    if (!IsWindowReady()) {
      throw std::runtime_error{"raylib visual test window failed to initialize"};
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

[[nodiscard]] std::filesystem::path asset_root() { return GISLAND_TEST_ASSET_ROOT; }

[[nodiscard]] std::filesystem::path baseline_root() { return GISLAND_TEST_BASELINE_ROOT; }

[[nodiscard]] std::filesystem::path artifact_root() { return GISLAND_VISUAL_ARTIFACT_ROOT; }

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    throw std::runtime_error{"failed to read " + path.string()};
  }
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] gisland::Theme load_theme() {
  const auto path = asset_root() / "themes/default.toml";
  auto theme = gisland::parse_theme(read_file(path), path.string());
  if (!theme) {
    throw std::runtime_error{"failed to parse default theme at " + theme.error().path + ": " +
                             theme.error().message};
  }
  if (theme->icons().at("calendar").codepoint != U'\uF133' ||
      theme->icons().at("chevron-left").codepoint != U'\uF053' ||
      theme->icons().at("chevron-right").codepoint != U'\uF054') {
    throw std::runtime_error{"default theme contains incorrect Font Awesome 6 codepoints"};
  }
  return std::move(*theme);
}

[[nodiscard]] gisland::SceneNode text(std::string value, std::string role = "body") {
  return gisland::SceneNode{gisland::Text{std::move(value), std::move(role)}};
}

[[nodiscard]] gisland::SceneNode icon(std::string name, std::string label) {
  return gisland::SceneNode{gisland::Icon{std::move(name), std::move(label)}};
}

[[nodiscard]] gisland::SceneNode all_primitives_gallery() {
  return gisland::SceneNode{gisland::Column{
      {text("Primitive gallery", "title"),
       gisland::SceneNode{
           gisland::Row{{icon("calendar", "Calendar"), text("Text and icon"),
                         gisland::SceneNode{gisland::Spacer{true, {}}}, text("Muted", "muted")},
                        "center",
                        "small"}},
       gisland::SceneNode{gisland::Progress{0.68, "Progress", "success"}},
       gisland::SceneNode{
           gisland::Row{{text("Indicators", "caption"),
                         gisland::SceneNode{gisland::Indicator{"success", "Available"}},
                         gisland::SceneNode{gisland::Indicator{"warning", "Delayed"}},
                         gisland::SceneNode{gisland::Indicator{"error", "Failed"}},
                         gisland::SceneNode{gisland::Indicator{"muted", "Inactive"}}},
                        "center",
                        "small"}},
       gisland::SceneNode{
           gisland::Row{{gisland::SceneNode{gisland::Spacer{false, "small"}},
                         gisland::SceneNode{gisland::Button{icon("chevron-left", "Previous"),
                                                            "previous", true, "Previous item"}},
                         gisland::SceneNode{gisland::Button{text("Disabled", "caption"), "disabled",
                                                            false, "Disabled action"}},
                         gisland::SceneNode{gisland::Button{icon("chevron-right", "Next"), "next",
                                                            true, "Next item"}}},
                        "center",
                        "small"}}},
      "start",
      "normal"}};
}

[[nodiscard]] gisland::SceneNode indicator_effects_gallery() {
  return gisland::SceneNode{gisland::Row{
      {text("Normal", "caption"), gisland::SceneNode{gisland::Indicator{"success", "Normal"}},
       text("Glow", "caption"),
       gisland::SceneNode{gisland::Indicator{"warning", "Glow", {gisland::IndicatorEffect::glow}}},
       text("Breathe", "caption"),
       gisland::SceneNode{gisland::Indicator{
           "error",
           "Glow and breathe",
           {gisland::IndicatorEffect::glow, gisland::IndicatorEffect::breathe}}}},
      "center",
      "small"}};
}

[[nodiscard]] gisland::SceneNode compact_time_date() {
  return gisland::SceneNode{
      gisland::Row{{text("14:32", "compact-primary"), gisland::SceneNode{gisland::Spacer{true, {}}},
                    text("ven. 31 juil.", "compact-secondary")},
                   "center",
                   "small"}};
}

[[nodiscard]] gisland::SceneNode compact_notification_image() {
  return gisland::SceneNode{gisland::Row{
      {gisland::SceneNode{gisland::Image{"app-icon", "notification-icon", "Example application"}},
       text("Image ready", "compact-primary"), gisland::SceneNode{gisland::Spacer{true, {}}},
       text("now", "compact-secondary")},
      "center",
      "small"}};
}

[[nodiscard]] gisland::SceneNode compact_rich_notification() {
  return gisland::SceneNode{
      gisland::Row{{gisland::SceneNode{gisland::Image{"app-icon", "notification-icon", "Files"}},
                    text("Download complete: archive.tar.gz is ready", "compact-primary")},
                   "center",
                   "small"}};
}

[[nodiscard]] gisland::SceneNode expanded_rich_notification() {
  return gisland::SceneNode{gisland::ActionRegion{
      gisland::SceneNode{gisland::Column{
          {gisland::SceneNode{gisland::Row{
               {gisland::SceneNode{gisland::Image{"app-icon", "notification-header-icon", "Files"}},
                gisland::SceneNode{
                    gisland::Column{{text("FILES", "caption"), text("Download complete", "body")},
                                    "start",
                                    "xsmall"}},
                gisland::SceneNode{gisland::Spacer{true, {}}},
                gisland::SceneNode{gisland::ActionRegion{icon("close", "Close notification"),
                                                         "close", true, "Close notification"}}},
               "center",
               "small"}},
           gisland::SceneNode{
               gisland::RichText{
                   .role = "notification-body",
                   .content =
                       {
                           gisland::RichTextSpan{"The file ", {}},
                           gisland::RichTextSpan{"archive.tar.gz", {gisland::TextEmphasis::bold}},
                           gisland::RichTextSpan{" is available in Downloads.\n", {}},
                           gisland::RichLinkSpan{
                               "Open the folder", {}, "open-folder", "Open the download folder"},
                           gisland::RichTextSpan{"\n", {}},
                           gisland::RichInlineImage{
                               "body-image", "notification-inline-image",
                               "Downloaded image preview"},
                       }}},
           gisland::SceneNode{gisland::Row{
               {gisland::SceneNode{
                    gisland::Button{text("Open", "button"), "open", true, "Open download"}},
                gisland::SceneNode{gisland::Button{text("Dismiss", "button"), "dismiss", true,
                                                   "Dismiss notification"}}},
               "center",
               "small"}}},
          "start",
          "small"}},
      "default", true, "Open notification"}};
}

[[nodiscard]] std::vector<gisland::ImageResource> notification_resources() {
  const auto pixels = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{
      255, 64,  64, 255, 255, 64,  64, 255, 64, 96,  255, 255, 64, 96,  255, 255,
      255, 192, 64, 255, 255, 192, 64, 255, 64, 224, 160, 255, 64, 224, 160, 255});
  return {gisland::ImageResource{"app-icon", gisland::ImageFormat::rgba8, 4, 2, pixels},
          gisland::ImageResource{"body-image", gisland::ImageFormat::rgba8, 4, 2, pixels}};
}

[[nodiscard]] gisland::SceneNode calendar_cell(std::string value, bool current = false) {
  return text(std::move(value), current ? "current" : "body");
}

[[nodiscard]] gisland::SceneNode calendar_row(const std::array<std::string_view, 7> &values,
                                              int current_day = -1) {
  std::vector<gisland::SceneNode> children;
  children.reserve(values.size());
  for (const auto value : values) {
    const int day = value.empty() ? -1 : std::atoi(std::string{value}.c_str());
    children.push_back(calendar_cell(std::string{value.empty() ? "  " : value},
                                     current_day > 0 && day == current_day));
  }
  return gisland::SceneNode{gisland::Row{std::move(children), "center", "large"}};
}

[[nodiscard]] gisland::SceneNode july_2026_calendar() {
  std::vector<gisland::SceneNode> children;
  children.emplace_back(gisland::Row{
      {gisland::SceneNode{gisland::Button{icon("chevron-left", "Previous month"), "previous-month",
                                          true, "Previous month"}},
       gisland::SceneNode{gisland::Spacer{true, {}}},
       gisland::SceneNode{
           gisland::Column{{text("Juillet 2026", "title"),
                            gisland::SceneNode{gisland::Button{text("Aujourd'hui", "button"),
                                                               "today", true, "Return to today"}}},
                           "center",
                           "xsmall"}},
       gisland::SceneNode{gisland::Spacer{true, {}}},
       gisland::SceneNode{
           gisland::Button{icon("chevron-right", "Next month"), "next-month", true, "Next month"}}},
      "center",
      "small"});
  children.push_back(calendar_row({"Lu", "Ma", "Me", "Je", "Ve", "Sa", "Di"}));
  children.push_back(calendar_row({"", "", "01", "02", "03", "04", "05"}));
  children.push_back(calendar_row({"06", "07", "08", "09", "10", "11", "12"}));
  children.push_back(calendar_row({"13", "14", "15", "16", "17", "18", "19"}));
  children.push_back(calendar_row({"20", "21", "22", "23", "24", "25", "26"}));
  children.push_back(calendar_row({"27", "28", "29", "30", "31", "", ""}, 31));
  return gisland::SceneNode{gisland::Column{std::move(children), "center", "small"}};
}

[[nodiscard]] gisland::SceneNode constrained_utf8() {
  return gisland::SceneNode{gisland::Text{"Caf\xC3\xA9 cr\xC3\xA8me, d\xC3\xA9j\xC3\xA0 vu, and a "
                                          "deliberately long ending that must be "
                                          "truncated safely",
                                          "body", "end"}};
}

[[nodiscard]] Image render_fixture(const gisland::SceneNode &scene, gisland::ViewMode mode,
                                   const std::vector<gisland::ImageResource> &resources = {},
                                   std::string_view compact_style = {},
                                   std::optional<float> progress_elapsed = std::nullopt,
                                   gisland::IndicatorAnimationState indicator_animation = {}) {
  const auto theme = load_theme();
  auto fonts = gisland::RaylibFontBook::load(theme, asset_root());
  REQUIRE(fonts.has_value());
  auto pango = gisland::PangoTextBook::load(theme, asset_root());
  REQUIRE(pango.has_value());
  const auto plan = gisland::layout_scene(scene, theme, mode, *fonts, *pango, compact_style);
  REQUIRE(plan.has_value());
  REQUIRE(plan->view.bounds.width <= image_width);
  REQUIRE(plan->view.bounds.height <= image_height);
  auto images = gisland::RaylibImageBook::load(resources);
  REQUIRE(images.has_value());
  REQUIRE(images->prepare(*plan).has_value());
  auto rich_textures = gisland::RaylibRichTextBook::load(*pango, resources);
  REQUIRE(rich_textures.has_value());
  REQUIRE(rich_textures->prepare(*plan).has_value());

  gisland::LayoutPlan rendered_plan = *plan;
  if (progress_elapsed) {
    gisland::ProgressAnimator animator;
    animator.retarget(rendered_plan, std::chrono::milliseconds{270}, gisland::Easing::ease_out,
                      false);
    animator.update(*progress_elapsed);
    rendered_plan = animator.apply(rendered_plan);
  }
  const gisland::RenderOrigin origin{(image_width - rendered_plan.view.bounds.width) / 2,
                                     (image_height - rendered_plan.view.bounds.height) / 2};
  const gisland::RaylibPainter painter{*fonts, *images, *rich_textures};
  RenderTexture2D target = LoadRenderTexture(image_width, image_height);
  REQUIRE(IsRenderTextureValid(target));
  BeginTextureMode(target);
  ClearBackground(BLANK);
  REQUIRE(painter.draw_surface(rendered_plan, origin).has_value());
  REQUIRE(painter.draw_content(rendered_plan, origin, indicator_animation).has_value());
  EndTextureMode();
  Image image = LoadImageFromTexture(target.texture);
  UnloadRenderTexture(target);
  ImageFlipVertical(&image);
  ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
  return image;
}

[[nodiscard]] int channel_delta(unsigned char left, unsigned char right) {
  return std::abs(static_cast<int>(left) - static_cast<int>(right));
}

void export_failure_artifacts(std::string_view name, const Image &actual, const Image &expected) {
  std::filesystem::create_directories(artifact_root());
  const auto actual_path = artifact_root() / (std::string{name} + "-actual.png");
  const auto diff_path = artifact_root() / (std::string{name} + "-diff.png");
  REQUIRE(ExportImage(actual, actual_path.c_str()));

  Image diff = GenImageColor(image_width, image_height, BLANK);
  for (int y = 0; y < image_height; ++y) {
    for (int x = 0; x < image_width; ++x) {
      const Color left = GetImageColor(actual, x, y);
      const Color right = GetImageColor(expected, x, y);
      const int delta = std::max({channel_delta(left.r, right.r), channel_delta(left.g, right.g),
                                  channel_delta(left.b, right.b), channel_delta(left.a, right.a)});
      if (delta > maximum_channel_delta) {
        ImageDrawPixel(&diff, x, y,
                       Color{255, static_cast<unsigned char>(255 - std::min(delta, 255)), 0, 255});
      }
    }
  }
  REQUIRE(ExportImage(diff, diff_path.c_str()));
  UnloadImage(diff);
}

void check_fixture(std::string_view name, const gisland::SceneNode &scene, gisland::ViewMode mode,
                   const std::vector<gisland::ImageResource> &resources = {},
                   std::string_view compact_style = {},
                   std::optional<float> progress_elapsed = std::nullopt,
                   gisland::IndicatorAnimationState indicator_animation = {}) {
  Image actual =
      render_fixture(scene, mode, resources, compact_style, progress_elapsed, indicator_animation);
  const auto baseline = baseline_root() / (std::string{name} + ".png");
  const char *update = std::getenv("GISLAND_UPDATE_BASELINES");
  const char *approved_update = std::getenv("GISLAND_BASELINE_UPDATE_TARGET");
  if (update != nullptr && std::string_view{update} == "1" && approved_update != nullptr &&
      std::string_view{approved_update} == "1") {
    std::filesystem::create_directories(baseline_root());
    REQUIRE(ExportImage(actual, baseline.c_str()));
    UnloadImage(actual);
    return;
  }

  INFO("Set GISLAND_UPDATE_BASELINES=1 to approve this fixture");
  REQUIRE(std::filesystem::exists(baseline));
  Image expected = LoadImage(baseline.c_str());
  REQUIRE(IsImageValid(expected));
  ImageFormat(&expected, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
  REQUIRE(expected.width == image_width);
  REQUIRE(expected.height == image_height);

  int divergent_pixels = 0;
  for (int y = 0; y < image_height; ++y) {
    for (int x = 0; x < image_width; ++x) {
      const Color left = GetImageColor(actual, x, y);
      const Color right = GetImageColor(expected, x, y);
      if (channel_delta(left.r, right.r) > maximum_channel_delta ||
          channel_delta(left.g, right.g) > maximum_channel_delta ||
          channel_delta(left.b, right.b) > maximum_channel_delta ||
          channel_delta(left.a, right.a) > maximum_channel_delta) {
        ++divergent_pixels;
      }
    }
  }
  if (divergent_pixels > maximum_divergent_pixels) {
    export_failure_artifacts(name, actual, expected);
  }
  INFO("divergent pixels: " << divergent_pixels << ", allowed: " << maximum_divergent_pixels);
  CHECK(divergent_pixels <= maximum_divergent_pixels);
  UnloadImage(expected);
  UnloadImage(actual);
}

} // namespace

TEST_CASE_METHOD(HiddenWindow, "visual regression: all v1 primitives gallery") {
  check_fixture("all-primitives", all_primitives_gallery(), gisland::ViewMode::expanded);
}

TEST_CASE_METHOD(HiddenWindow, "visual regression: compact time and date capsule") {
  check_fixture("compact-time-date", compact_time_date(), gisland::ViewMode::compact);
}

TEST_CASE_METHOD(HiddenWindow, "visual regression: semantic indicator effects") {
  check_fixture("indicator-effects", indicator_effects_gallery(), gisland::ViewMode::compact, {},
                {}, std::nullopt, {0.8, false});
}

TEST_CASE_METHOD(HiddenWindow, "visual regression: dynamic image cropped into a circle") {
  check_fixture("dynamic-image-circle", compact_notification_image(), gisland::ViewMode::compact,
                notification_resources());
}

TEST_CASE_METHOD(HiddenWindow, "visual regression: compact rich notification") {
  const auto theme = load_theme();
  auto fonts = gisland::RaylibFontBook::load(theme, asset_root());
  REQUIRE(fonts.has_value());
  auto pango = gisland::PangoTextBook::load(theme, asset_root());
  REQUIRE(pango.has_value());
  const auto plan = gisland::layout_scene(compact_rich_notification(), theme,
                                          gisland::ViewMode::compact, *fonts, *pango);
  REQUIRE(plan.has_value());
  const auto image = std::ranges::find_if(plan->content, [](const auto &command) {
    return std::holds_alternative<gisland::ImageDrawCommand>(command);
  });
  REQUIRE(image != plan->content.end());
  CHECK(std::get<gisland::ImageDrawCommand>(*image).bounds == gisland::Rect{4, 4, 24, 24});

  check_fixture("notification-compact", compact_rich_notification(), gisland::ViewMode::compact,
                notification_resources());
}

TEST_CASE_METHOD(HiddenWindow, "visual regression: expanded rich notification") {
  const auto theme = load_theme();
  auto fonts = gisland::RaylibFontBook::load(theme, asset_root());
  REQUIRE(fonts.has_value());
  auto pango = gisland::PangoTextBook::load(theme, asset_root());
  REQUIRE(pango.has_value());
  const auto plan = gisland::layout_scene(expanded_rich_notification(), theme,
                                          gisland::ViewMode::expanded, *fonts, *pango);
  REQUIRE(plan.has_value());

  std::vector<gisland::Rect> images;
  int button_decorations = 0;
  for (const auto &command : plan->content) {
    if (const auto *image = std::get_if<gisland::ImageDrawCommand>(&command); image != nullptr) {
      images.push_back(image->bounds);
    }
    button_decorations +=
        std::holds_alternative<gisland::ButtonDecorationDrawCommand>(command) ? 1 : 0;
  }
  REQUIRE(images.size() == 1);
  CHECK(images.front().width == 32);
  CHECK(images.front().height == 32);
  CHECK(button_decorations == 2);
  const auto rich = std::ranges::find_if(plan->content, [](const auto &command) {
    return std::holds_alternative<gisland::RichTextDrawCommand>(command);
  });
  REQUIRE(rich != plan->content.end());
  const auto &rich_command = std::get<gisland::RichTextDrawCommand>(*rich);
  REQUIRE(rich_command.composition.images.size() == 1);
  CHECK(rich_command.composition.images.front().bounds.width == 96);
  CHECK(rich_command.composition.images.front().bounds.height == 54);

  Image rendered = render_fixture(expanded_rich_notification(), gisland::ViewMode::expanded,
                                  notification_resources());
  const gisland::RenderOrigin origin{(image_width - plan->view.bounds.width) / 2,
                                     (image_height - plan->view.bounds.height) / 2};
  const auto close = std::ranges::find_if(plan->content, [](const auto &command) {
    const auto *candidate = std::get_if<gisland::IconDrawCommand>(&command);
    return candidate != nullptr && candidate->accessible_label == "Close notification";
  });
  REQUIRE(close != plan->content.end());
  const auto &close_bounds = std::get<gisland::IconDrawCommand>(*close).bounds;
  const auto surface = GetImageColor(rendered, origin.x + close_bounds.x - 3,
                                     origin.y + close_bounds.y + (close_bounds.height / 2));
  CHECK(surface.r == 0);
  CHECK(surface.g == 0);
  CHECK(surface.b == 0);
  CHECK(surface.a == 255);
  const auto separator = GetImageColor(rendered, origin.x + (plan->view.bounds.width / 2),
                                       origin.y + images.front().y + images.front().height + 4);
  CHECK(separator.r == 0);
  CHECK(separator.g == 0);
  CHECK(separator.b == 0);
  CHECK(separator.a == 255);
  UnloadImage(rendered);

  check_fixture("notification-expanded", expanded_rich_notification(), gisland::ViewMode::expanded,
                notification_resources());
}

TEST_CASE_METHOD(HiddenWindow, "visual regression: expanded July 2026 calendar") {
  check_fixture("calendar-july-2026", july_2026_calendar(), gisland::ViewMode::expanded);
}

TEST_CASE_METHOD(HiddenWindow, "visual regression: constrained UTF-8 truncation") {
  check_fixture("utf8-truncation", constrained_utf8(), gisland::ViewMode::compact);
}

TEST_CASE_METHOD(HiddenWindow, "visual regression: audio mute HUD") {
  gisland::test::AudioProcess audio{
      {{"volume_reads", {{50}, {50}}}, {"mute_reads", {false, true}}}};
  const auto publication = audio.action("toggle-mute");
  REQUIRE(publication.compact.has_value());
  REQUIRE(publication.presentation.has_value());
  REQUIRE(publication.presentation->compact_style.has_value());
  check_fixture("audio-muted", *publication.compact, gisland::ViewMode::compact, {},
                *publication.presentation->compact_style);
}

TEST_CASE_METHOD(HiddenWindow, "visual regression: audio unmute HUD") {
  gisland::test::AudioProcess audio{
      {{"volume_reads", {{50}, {50}}}, {"mute_reads", {true, false}}}};
  const auto publication = audio.action("toggle-mute");
  REQUIRE(publication.compact.has_value());
  REQUIRE(publication.presentation.has_value());
  REQUIRE(publication.presentation->compact_style.has_value());
  check_fixture("audio-unmuted", *publication.compact, gisland::ViewMode::compact, {},
                *publication.presentation->compact_style);
}

TEST_CASE_METHOD(HiddenWindow, "visual regression: audio volume animation") {
  gisland::test::AudioProcess audio{
      {{"volume_reads", {{30}, {120}}}, {"mute_reads", {false, false}}}};
  const auto publication = audio.action("volume-up");
  REQUIRE(publication.compact.has_value());
  REQUIRE(publication.presentation.has_value());
  REQUIRE(publication.presentation->compact_style.has_value());
  check_fixture("audio-volume-source", *publication.compact, gisland::ViewMode::compact, {},
                *publication.presentation->compact_style, 0.0F);
  check_fixture("audio-volume-intermediate", *publication.compact, gisland::ViewMode::compact, {},
                *publication.presentation->compact_style, 0.135F);
  check_fixture("audio-volume-settled", *publication.compact, gisland::ViewMode::compact, {},
                *publication.presentation->compact_style, 0.27F);
}
