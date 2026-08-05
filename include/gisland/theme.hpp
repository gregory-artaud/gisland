#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
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
};

using ThemeColor = std::variant<std::string, Rgba>;

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
};

struct AnimationStyle {
  std::chrono::milliseconds compact_to_expanded_ms;
  std::chrono::milliseconds context_change_ms;
  Easing easing;
  ReducedMotionStyle reduced_motion;
};

struct IconGlyph {
  std::string font;
  char32_t codepoint;
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

  [[nodiscard]] const Palette &palette() const noexcept { return palette_; }
  [[nodiscard]] const Typography &typography() const noexcept { return typography_; }
  [[nodiscard]] const PixelTokens &gaps() const noexcept { return gaps_; }
  [[nodiscard]] const PixelTokens &spacers() const noexcept { return spacers_; }
  [[nodiscard]] const ThemeViews &views() const noexcept { return views_; }
  [[nodiscard]] const ShadowStyle &shadow() const noexcept { return shadow_; }
  [[nodiscard]] const AnimationStyle &animation() const noexcept { return animation_; }
  [[nodiscard]] const FontResources &fonts() const noexcept { return fonts_; }
  [[nodiscard]] const Icons &icons() const noexcept { return icons_; }

private:
  Theme(Palette palette, Typography typography, PixelTokens gaps, PixelTokens spacers,
        ThemeViews views, ShadowStyle shadow, AnimationStyle animation, FontResources fonts,
        Icons icons);

  Palette palette_;
  Typography typography_;
  PixelTokens gaps_;
  PixelTokens spacers_;
  ThemeViews views_;
  ShadowStyle shadow_;
  AnimationStyle animation_;
  FontResources fonts_;
  Icons icons_;

  friend std::expected<Theme, ThemeError> parse_theme(std::string_view text,
                                                      std::string_view source_name);
};

[[nodiscard]] std::expected<Theme, ThemeError> parse_theme(std::string_view text,
                                                           std::string_view source_name);

} // namespace gisland
