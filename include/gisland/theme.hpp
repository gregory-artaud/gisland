#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace gisland {

struct Rgba {
  std::uint8_t red;
  std::uint8_t green;
  std::uint8_t blue;
  std::uint8_t alpha;

  bool operator==(const Rgba &) const = default;
};

struct TypographyRole {
  std::string font;
  std::string color;
  double size;
  std::uint16_t weight;
  double line_height;
};

struct ViewGeometry {
  double padding_horizontal;
  double padding_vertical;
  double radius;
  double border;
  double min_width;
  double max_width;
  double min_height;
  double max_height;
};

struct ThemeViews {
  ViewGeometry compact;
  ViewGeometry expanded;
  std::map<std::string, ViewGeometry, std::less<>> compact_styles;
};

using ThemeColor = std::variant<std::string, Rgba>;

struct ButtonStyle {
  ThemeColor background;
  ThemeColor disabled_background;
  ThemeColor hover_overlay;
};

struct ProgressStyle {
  double ring_diameter;
  double ring_thickness;
  double linear_thickness;
  double compact_height;
  ThemeColor track;
  std::optional<double> ring_track_opacity;
};

struct IndicatorStyle {
  double diameter;
};

struct ShadowStyle {
  double offset_x;
  double offset_y;
  double blur;
  double spread;
  ThemeColor color;
};

enum class Easing { linear, ease_in, ease_out, ease_in_out };

struct ReducedMotionStyle {
  std::chrono::milliseconds compact_to_expanded_ms;
  std::chrono::milliseconds context_change_ms;
  std::chrono::milliseconds progress_duration;
};

struct ProgressAnimationStyle {
  std::chrono::milliseconds duration;
  Easing easing;
};

struct AnimationStyle {
  std::chrono::milliseconds compact_to_expanded_ms;
  std::chrono::milliseconds context_change_ms;
  Easing easing;
  ProgressAnimationStyle progress;
  ReducedMotionStyle reduced_motion;
};

struct IconGlyph {
  std::string font;
  char32_t codepoint;
};

enum class ImageFit { contain, cover };
enum class ImageShape { rectangle, rounded, circle };
enum class ImagePlacement { flow, leading_cap };

struct ImageRole {
  double width;
  double height;
  ImageFit fit;
  ImageShape shape;
  double radius;
  ImagePlacement placement{ImagePlacement::flow};

  bool operator==(const ImageRole &) const = default;
};

struct ThemeError {
  std::string source;
  std::string path;
  std::string message;
  std::size_t line;
  std::size_t column;
};

class Theme final {
public:
  using Palette = std::map<std::string, Rgba>;
  using Typography = std::map<std::string, TypographyRole>;
  using PixelTokens = std::map<std::string, double>;
  using FontResources = std::map<std::string, std::string>;
  using Icons = std::map<std::string, IconGlyph>;
  using ImageRoles = std::map<std::string, ImageRole>;

  [[nodiscard]] const Palette &palette() const noexcept { return palette_; }
  [[nodiscard]] const Typography &typography() const noexcept { return typography_; }
  [[nodiscard]] const PixelTokens &gaps() const noexcept { return gaps_; }
  [[nodiscard]] const PixelTokens &spacers() const noexcept { return spacers_; }
  [[nodiscard]] const ThemeViews &views() const noexcept { return views_; }
  [[nodiscard]] const ButtonStyle &buttons() const noexcept { return buttons_; }
  [[nodiscard]] const ProgressStyle &progress() const noexcept { return progress_; }
  [[nodiscard]] const IndicatorStyle &indicator() const noexcept { return indicator_; }
  [[nodiscard]] const ShadowStyle &shadow() const noexcept { return shadow_; }
  [[nodiscard]] const AnimationStyle &animation() const noexcept { return animation_; }
  [[nodiscard]] const FontResources &fonts() const noexcept { return fonts_; }
  [[nodiscard]] const Icons &icons() const noexcept { return icons_; }
  [[nodiscard]] const ImageRoles &images() const noexcept { return images_; }

private:
  Theme(Palette palette, Typography typography, PixelTokens gaps, PixelTokens spacers,
        ThemeViews views, ButtonStyle buttons, ProgressStyle progress, IndicatorStyle indicator,
        ShadowStyle shadow, AnimationStyle animation, FontResources fonts, Icons icons,
        ImageRoles images);

  Palette palette_;
  Typography typography_;
  PixelTokens gaps_;
  PixelTokens spacers_;
  ThemeViews views_;
  ButtonStyle buttons_;
  ProgressStyle progress_;
  IndicatorStyle indicator_;
  ShadowStyle shadow_;
  AnimationStyle animation_;
  FontResources fonts_;
  Icons icons_;
  ImageRoles images_;

  friend std::expected<Theme, ThemeError> parse_theme(std::string_view text,
                                                      std::string_view source_name);
};

[[nodiscard]] std::expected<Theme, ThemeError> parse_theme(std::string_view text,
                                                           std::string_view source_name);
[[nodiscard]] Rgba resolve_theme_color(const Theme &theme, const ThemeColor &value);

} // namespace gisland
