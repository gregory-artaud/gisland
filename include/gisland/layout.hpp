#pragma once

#include "gisland/rich_text.hpp"
#include "gisland/scene.hpp"
#include "gisland/theme.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace gisland {

enum class ViewMode { compact, expanded };

struct MeasuredGlyphs {
  double width;
  double height;
};

class GlyphMetrics {
public:
  virtual ~GlyphMetrics() = default;

  [[nodiscard]] virtual bool supports_text(std::string_view /*font_resource*/,
                                           const TypographyRole & /*role*/,
                                           std::string_view /*text*/) const {
    return true;
  }
  [[nodiscard]] virtual bool supports_codepoint(std::string_view /*font_resource*/,
                                                const TypographyRole & /*role*/,
                                                char32_t /*codepoint*/) const {
    return true;
  }
  [[nodiscard]] virtual MeasuredGlyphs measure_text(std::string_view font_resource,
                                                    const TypographyRole &role,
                                                    std::string_view text) const = 0;
  [[nodiscard]] virtual MeasuredGlyphs measure_codepoint(std::string_view font_resource,
                                                         const TypographyRole &role,
                                                         char32_t codepoint) const = 0;
};

struct Rect {
  int x;
  int y;
  int width;
  int height;

  bool operator==(const Rect &) const = default;
};

struct RectInsets {
  int left{};
  int top{};
  int right{};
  int bottom{};

  bool operator==(const RectInsets &) const = default;
};

struct ViewShadow {
  int offset_x{};
  int offset_y{};
  int blur{};
  int spread{};
  Rgba color{};
};

struct RoundedView {
  Rect bounds;
  int radius;
  int border;
  Rgba surface;
  Rgba border_color;
  ViewShadow shadow{};
};

[[nodiscard]] RectInsets shadow_insets(const RoundedView &view);

struct TextDrawCommand {
  Rect bounds;
  Rect clip;
  std::string text;
  std::string font_resource;
  TypographyRole typography;
  Rgba color;
};

struct RichTextDrawCommand {
  Rect bounds;
  Rect clip;
  RichText rich_text;
  RichTextComposition composition;
  std::string font_resource;
  TypographyRole typography;
  Rgba color;
  Rgba accent_color;
};

struct IconDrawCommand {
  Rect bounds;
  Rect clip;
  std::string font_resource;
  TypographyRole typography;
  char32_t codepoint;
  Rgba color;
  std::string accessible_label;
};

struct ImageDrawCommand {
  Rect bounds;
  Rect clip;
  std::string resource_id;
  ImageRole style;
  std::string accessible_label;
};

struct ProgressDrawCommand {
  Rect bounds;
  Rect clip;
  Rect track;
  Rect fill;
  Rgba track_color;
  Rgba fill_color;
  std::string path;
  double value;
  std::optional<double> transition_from;
};

struct RingProgressDrawCommand {
  Rect bounds;
  Rect clip;
  double value;
  int thickness;
  Rgba track_color;
  Rgba fill_color;
  std::string path;
  std::optional<double> transition_from;
};

struct IndicatorDrawCommand {
  Rect bounds;
  Rect clip;
  Rgba color;
  std::string accessible_label;
};

struct ButtonDecorationDrawCommand {
  Rect bounds;
  Rect clip;
  Rgba color;
  bool enabled;
};

using ContentDrawCommand =
    std::variant<TextDrawCommand, RichTextDrawCommand, IconDrawCommand, ImageDrawCommand,
                 ProgressDrawCommand, RingProgressDrawCommand, IndicatorDrawCommand,
                 ButtonDecorationDrawCommand>;

enum class InteractionKind { button, link, action_region };

struct InteractionTarget {
  Rect bounds;
  Rect clip;
  std::string action_id;
  bool enabled;
  std::string accessible_label;
  InteractionKind kind{InteractionKind::action_region};

  bool operator==(const InteractionTarget &) const = default;
};

struct LayoutPlan {
  RoundedView view;
  std::vector<ContentDrawCommand> content;
  std::vector<InteractionTarget> interactions{};
};

enum class LayoutErrorCode {
  unknown_role,
  unknown_compact_style,
  unknown_image_role,
  invalid_image_placement,
  unknown_gap,
  unknown_spacer,
  unknown_icon,
  unknown_alignment,
  unknown_truncation,
  invalid_utf8,
  unsupported_glyph,
  impossible_constraints
};

struct LayoutError {
  LayoutErrorCode code;
  std::string path;
  std::string message;
};

[[nodiscard]] std::expected<LayoutPlan, LayoutError>
layout_scene(const SceneNode &scene, const Theme &theme, ViewMode mode,
             const GlyphMetrics &glyph_metrics, std::string_view compact_style = {});
[[nodiscard]] std::expected<LayoutPlan, LayoutError>
layout_scene(const SceneNode &scene, const Theme &theme, ViewMode mode,
             const GlyphMetrics &glyph_metrics, const RichTextMetrics &rich_text_metrics,
             std::string_view compact_style = {});

} // namespace gisland
