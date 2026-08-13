#include "gisland/layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace gisland {
namespace {

enum class Axis { horizontal, vertical };
enum class Alignment { start, center, end };

struct MeasuredNode {
  MeasuredNode(const SceneNode *source, std::string path_value)
      : scene(source), path(std::move(path_value)) {}

  const SceneNode *scene{};
  std::string path;
  int width{};
  int height{};
  int minimum_width{};
  int minimum_height{};
  int gap{};
  int token_size{};
  int track_width{};
  int track_height{};
  int label_width{};
  int label_height{};
  int padding{};
  bool flexible{};
  bool expands_width{};
  bool expands_height{};
  Alignment alignment{Alignment::center};
  const TypographyRole *typography{};
  const IconGlyph *icon{};
  const ImageRole *image_role{};
  std::string text;
  std::string font_resource;
  Rgba color{};
  std::optional<RichTextComposition> rich_composition;
  std::vector<MeasuredNode> children;
};

[[nodiscard]] bool contains_ring_progress(const SceneNode &scene) {
  return std::visit(
      [](const auto &node) {
        using Node = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<Node, Progress>) {
          return node.shape == ProgressShape::ring;
        } else if constexpr (std::is_same_v<Node, Row> || std::is_same_v<Node, Column>) {
          return std::any_of(
              node.children.begin(), node.children.end(),
              [](const SceneChild &child) { return contains_ring_progress(*child); });
        } else if constexpr (std::is_same_v<Node, Button> || std::is_same_v<Node, ActionRegion>) {
          return contains_ring_progress(*node.content);
        }
        return false;
      },
      scene.value);
}

[[nodiscard]] LayoutError error(LayoutErrorCode code, std::string path, std::string message) {
  return LayoutError{code, std::move(path), std::move(message)};
}

[[nodiscard]] std::expected<int, LayoutError> rounded_pixel(double value, const std::string &path) {
  if (!std::isfinite(value) || value < 0.0 ||
      value > static_cast<double>(std::numeric_limits<int>::max())) {
    return std::unexpected(error(LayoutErrorCode::impossible_constraints, path,
                                 "measurement is not a finite non-negative pixel value"));
  }
  return static_cast<int>(std::lround(value));
}

[[nodiscard]] std::expected<int, LayoutError> rounded_signed_pixel(double value,
                                                                   const std::string &path) {
  if (!std::isfinite(value) || value < static_cast<double>(std::numeric_limits<int>::min()) ||
      value > static_cast<double>(std::numeric_limits<int>::max())) {
    return std::unexpected(error(LayoutErrorCode::impossible_constraints, path,
                                 "measurement is not a finite pixel value"));
  }
  return static_cast<int>(std::lround(value));
}

[[nodiscard]] std::expected<int, LayoutError> checked_add(int left, int right,
                                                          const std::string &path) {
  const auto result = static_cast<long long>(left) + static_cast<long long>(right);
  if (result < std::numeric_limits<int>::min() || result > std::numeric_limits<int>::max()) {
    return std::unexpected(
        error(LayoutErrorCode::impossible_constraints, path, "pixel addition overflows"));
  }
  return static_cast<int>(result);
}

[[nodiscard]] std::expected<int, LayoutError> checked_subtract(int left, int right,
                                                               const std::string &path) {
  const auto result = static_cast<long long>(left) - static_cast<long long>(right);
  if (result < std::numeric_limits<int>::min() || result > std::numeric_limits<int>::max()) {
    return std::unexpected(
        error(LayoutErrorCode::impossible_constraints, path, "pixel subtraction overflows"));
  }
  return static_cast<int>(result);
}

[[nodiscard]] std::expected<int, LayoutError> checked_multiply(int left, int right,
                                                               const std::string &path) {
  const auto result = static_cast<long long>(left) * static_cast<long long>(right);
  if (result < std::numeric_limits<int>::min() || result > std::numeric_limits<int>::max()) {
    return std::unexpected(
        error(LayoutErrorCode::impossible_constraints, path, "pixel multiplication overflows"));
  }
  return static_cast<int>(result);
}

[[nodiscard]] std::expected<Rect, LayoutError> intersect(Rect left, Rect right,
                                                         const std::string &path) {
  const int x = std::max(left.x, right.x);
  const int y = std::max(left.y, right.y);
  auto left_right = checked_add(left.x, left.width, path);
  auto right_right = checked_add(right.x, right.width, path);
  auto left_bottom = checked_add(left.y, left.height, path);
  auto right_bottom = checked_add(right.y, right.height, path);
  if (!left_right) {
    return std::unexpected(left_right.error());
  }
  if (!right_right) {
    return std::unexpected(right_right.error());
  }
  if (!left_bottom) {
    return std::unexpected(left_bottom.error());
  }
  if (!right_bottom) {
    return std::unexpected(right_bottom.error());
  }
  auto width = checked_subtract(std::min(*left_right, *right_right), x, path);
  auto height = checked_subtract(std::min(*left_bottom, *right_bottom), y, path);
  if (!width) {
    return std::unexpected(width.error());
  }
  if (!height) {
    return std::unexpected(height.error());
  }
  return Rect{x, y, std::max(0, *width), std::max(0, *height)};
}

[[nodiscard]] std::string one_line(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '\r' || text[index] == '\n') {
      result.push_back(' ');
      if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n') {
        ++index;
      }
    } else {
      result.push_back(text[index]);
    }
  }
  return result;
}

[[nodiscard]] bool valid_utf8(std::string_view text) {
  for (std::size_t index = 0; index < text.size();) {
    const auto first = static_cast<unsigned char>(text[index]);
    std::size_t length = 0;
    if (first <= 0x7FU) {
      length = 1;
    } else if (first >= 0xC2U && first <= 0xDFU) {
      length = 2;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      length = 3;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      length = 4;
    } else {
      return false;
    }
    if (index + length > text.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto continuation = static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xC0U) != 0x80U) {
        return false;
      }
    }
    if (length == 3) {
      const auto second = static_cast<unsigned char>(text[index + 1]);
      if ((first == 0xE0U && second < 0xA0U) || (first == 0xEDU && second > 0x9FU)) {
        return false;
      }
    } else if (length == 4) {
      const auto second = static_cast<unsigned char>(text[index + 1]);
      if ((first == 0xF0U && second < 0x90U) || (first == 0xF4U && second > 0x8FU)) {
        return false;
      }
    }
    index += length;
  }
  return true;
}

[[nodiscard]] int main_size(const MeasuredNode &node, Axis axis, bool minimum) {
  if (const auto *spacer = std::get_if<Spacer>(&node.scene->value); spacer != nullptr) {
    if (spacer->flexible) {
      return 0;
    }
    return node.token_size;
  }
  if (axis == Axis::horizontal) {
    return minimum ? node.minimum_width : node.width;
  }
  return minimum ? node.minimum_height : node.height;
}

[[nodiscard]] int cross_size(const MeasuredNode &node, Axis axis, bool minimum) {
  if (std::holds_alternative<Spacer>(node.scene->value)) {
    return 0;
  }
  if (axis == Axis::horizontal) {
    return minimum ? node.minimum_height : node.height;
  }
  return minimum ? node.minimum_width : node.width;
}

[[nodiscard]] bool expands_main(const MeasuredNode &node, Axis axis) {
  return node.flexible || (axis == Axis::horizontal ? node.expands_width : node.expands_height);
}

class LayoutBuilder {
public:
  LayoutBuilder(const Theme &theme, const GlyphMetrics &metrics,
                const RichTextMetrics *rich_metrics, ViewMode mode, bool root_leading_cap)
      : theme_(theme), metrics_(metrics), rich_metrics_(rich_metrics), mode_(mode),
        root_leading_cap_(root_leading_cap) {}

  [[nodiscard]] std::expected<MeasuredNode, LayoutError> measure(const SceneNode &scene,
                                                                 std::string path) const {
    return std::visit(
        [this, &scene, &path](const auto &primitive) {
          return measure_primitive(scene, primitive, path);
        },
        scene.value);
  }

  [[nodiscard]] std::expected<void, LayoutError>
  place(const MeasuredNode &node, Rect assigned, Rect clip,
        std::vector<ContentDrawCommand> &commands,
        std::vector<InteractionTarget> &interactions) const {
    return std::visit(
        [this, &node, assigned, clip, &commands, &interactions](const auto &primitive) {
          return place_primitive(node, primitive, assigned, clip, commands, interactions);
        },
        node.scene->value);
  }

  [[nodiscard]] std::expected<void, LayoutError> constrain_width(MeasuredNode &node,
                                                                 int available_width) const {
    if (available_width < 0) {
      return std::unexpected(error(LayoutErrorCode::impossible_constraints, node.path,
                                   "node has a negative assigned width"));
    }
    return std::visit(
        [this, &node, available_width](const auto &primitive) {
          return constrain_primitive(node, primitive, available_width);
        },
        node.scene->value);
  }

private:
  [[nodiscard]] static LayoutError rich_error(const RichTextError &rich, const std::string &path) {
    LayoutErrorCode code = LayoutErrorCode::impossible_constraints;
    if (rich.code == RichTextErrorCode::invalid_utf8) {
      code = LayoutErrorCode::invalid_utf8;
    } else if (rich.code == RichTextErrorCode::unknown_role) {
      code = LayoutErrorCode::unknown_role;
    } else if (rich.code == RichTextErrorCode::unknown_image_role) {
      code = LayoutErrorCode::unknown_image_role;
    } else if (rich.code == RichTextErrorCode::unsupported_glyph) {
      code = LayoutErrorCode::unsupported_glyph;
    }
    return error(code, path + rich.path, rich.message);
  }

  [[nodiscard]] std::expected<const TypographyRole *, LayoutError>
  typography(std::string_view role, const std::string &path) const {
    const auto iterator = theme_.typography().find(std::string{role});
    if (iterator == theme_.typography().end()) {
      return std::unexpected(error(LayoutErrorCode::unknown_role, path, "unknown typography role"));
    }
    return &iterator->second;
  }

  [[nodiscard]] std::expected<MeasuredNode, LayoutError>
  measure_primitive(const SceneNode &scene, const Image &image, const std::string &path) const {
    const auto role = theme_.images().find(image.role);
    if (role == theme_.images().end()) {
      return std::unexpected(error(LayoutErrorCode::unknown_image_role, path + "/role",
                                   "unknown semantic image role"));
    }
    if (role->second.placement == ImagePlacement::leading_cap &&
        (mode_ != ViewMode::compact || !root_leading_cap_ || path != "/children/0")) {
      return std::unexpected(
          error(LayoutErrorCode::invalid_image_placement, path + "/role",
                "leading-cap image must be the first child of the compact root row"));
    }
    auto width = rounded_pixel(role->second.width, path + "/role");
    auto height = rounded_pixel(role->second.height, path + "/role");
    if (!width) {
      return std::unexpected(width.error());
    }
    if (!height) {
      return std::unexpected(height.error());
    }
    MeasuredNode result{&scene, path};
    result.width = *width;
    result.height = *height;
    result.minimum_width = *width;
    result.minimum_height = *height;
    result.image_role = &role->second;
    return result;
  }

  [[nodiscard]] std::expected<std::string, LayoutError>
  font_resource(std::string_view font, const std::string &path) const {
    const auto iterator = theme_.fonts().find(std::string{font});
    if (iterator == theme_.fonts().end()) {
      return std::unexpected(
          error(LayoutErrorCode::unknown_role, path, "typography references an unknown font"));
    }
    return iterator->second;
  }

  [[nodiscard]] std::expected<std::pair<int, int>, LayoutError>
  measure_text(std::string_view font, const TypographyRole &role, std::string_view text,
               const std::string &path) const {
    if (!valid_utf8(text)) {
      return std::unexpected(error(LayoutErrorCode::invalid_utf8, path, "text is not valid UTF-8"));
    }
    if (!metrics_.supports_text(font, role, text)) {
      return std::unexpected(error(LayoutErrorCode::unsupported_glyph, path,
                                   "font does not support every text codepoint"));
    }
    const auto measured = metrics_.measure_text(font, role, text);
    auto width = rounded_pixel(measured.width, path);
    auto height = rounded_pixel(measured.height, path);
    if (!width) {
      return std::unexpected(width.error());
    }
    if (!height) {
      return std::unexpected(height.error());
    }
    return std::pair{*width, *height};
  }

  [[nodiscard]] std::expected<Alignment, LayoutError> alignment(std::string_view value,
                                                                const std::string &path) const {
    if (value == "start") {
      return Alignment::start;
    }
    if (value == "center") {
      return Alignment::center;
    }
    if (value == "end") {
      return Alignment::end;
    }
    return std::unexpected(
        error(LayoutErrorCode::unknown_alignment, path, "unknown cross-axis alignment"));
  }

  [[nodiscard]] std::expected<int, LayoutError> token(const Theme::PixelTokens &tokens,
                                                      std::string_view value, LayoutErrorCode code,
                                                      const std::string &path) const {
    const auto iterator = tokens.find(std::string{value});
    if (iterator == tokens.end()) {
      return std::unexpected(error(code, path, "unknown semantic pixel token"));
    }
    return rounded_pixel(iterator->second, path);
  }

  [[nodiscard]] std::expected<MeasuredNode, LayoutError>
  measure_primitive(const SceneNode &scene, const Text &text, const std::string &path) const {
    if (text.truncation != "end" && text.truncation != "clip") {
      return std::unexpected(error(LayoutErrorCode::unknown_truncation, path + "/truncation",
                                   "unknown text truncation policy"));
    }
    auto role = typography(text.role, path + "/role");
    if (!role) {
      return std::unexpected(role.error());
    }
    auto font = font_resource((*role)->font, path + "/role");
    if (!font) {
      return std::unexpected(font.error());
    }
    const std::string resolved_text = one_line(text.value);
    auto size = measure_text(*font, **role, resolved_text, path + "/value");
    if (!size) {
      return std::unexpected(size.error());
    }
    int minimum_width = 0;
    if (text.truncation == "end" && !resolved_text.empty()) {
      auto ellipsis = measure_text(*font, **role, "\xE2\x80\xA6", path + "/value");
      if (!ellipsis) {
        return std::unexpected(ellipsis.error());
      }
      minimum_width = ellipsis->first;
    }
    MeasuredNode result{&scene, path};
    result.width = size->first;
    result.height = size->second;
    result.minimum_width = std::min(result.width, minimum_width);
    result.minimum_height = result.height;
    result.typography = *role;
    result.text = resolved_text;
    result.font_resource = std::move(*font);
    result.color = theme_.palette().at((*role)->color);
    return result;
  }

  [[nodiscard]] std::expected<MeasuredNode, LayoutError>
  measure_primitive(const SceneNode &scene, const RichText &rich, const std::string &path) const {
    auto role = typography(rich.role, path + "/role");
    if (!role) {
      return std::unexpected(role.error());
    }
    auto font = font_resource((*role)->font, path + "/role");
    if (!font) {
      return std::unexpected(font.error());
    }
    if (rich_metrics_ == nullptr) {
      return std::unexpected(error(LayoutErrorCode::impossible_constraints, path,
                                   "rich text metrics are unavailable"));
    }
    auto minimum = rich_metrics_->compose(rich, 1);
    if (!minimum) {
      return std::unexpected(rich_error(minimum.error(), path));
    }
    const int natural_width = std::max(1, minimum->natural_width);
    auto natural = rich_metrics_->compose(rich, natural_width);
    if (!natural) {
      return std::unexpected(rich_error(natural.error(), path));
    }
    if (minimum->minimum_width < 0 || natural->height < 0) {
      return std::unexpected(error(LayoutErrorCode::impossible_constraints, path,
                                   "rich text metrics must be non-negative"));
    }
    MeasuredNode result{&scene, path};
    result.width = natural_width;
    result.height = natural->height;
    result.minimum_width = minimum->minimum_width;
    result.minimum_height = natural->height;
    result.expands_width = true;
    result.typography = *role;
    result.font_resource = std::move(*font);
    result.color = theme_.palette().at((*role)->color);
    result.rich_composition = std::move(*natural);
    return result;
  }

  [[nodiscard]] std::expected<MeasuredNode, LayoutError>
  measure_primitive(const SceneNode &scene, const Icon &icon, const std::string &path) const {
    const auto icon_iterator = theme_.icons().find(icon.name);
    if (icon_iterator == theme_.icons().end()) {
      return std::unexpected(
          error(LayoutErrorCode::unknown_icon, path + "/name", "unknown semantic icon"));
    }
    auto role = typography(icon.role, path + "/role");
    if (!role) {
      return std::unexpected(role.error());
    }
    auto font = font_resource(icon_iterator->second.font, path + "/name");
    if (!font) {
      return std::unexpected(font.error());
    }
    if (!metrics_.supports_codepoint(*font, **role, icon_iterator->second.codepoint)) {
      return std::unexpected(error(LayoutErrorCode::unsupported_glyph, path + "/name",
                                   "icon font does not support the configured codepoint"));
    }
    const auto measured =
        metrics_.measure_codepoint(*font, **role, icon_iterator->second.codepoint);
    auto width = rounded_pixel(measured.width, path);
    auto height = rounded_pixel(measured.height, path);
    if (!width) {
      return std::unexpected(width.error());
    }
    if (!height) {
      return std::unexpected(height.error());
    }
    MeasuredNode result{&scene, path};
    result.width = *width;
    result.height = *height;
    result.minimum_width = *width;
    result.minimum_height = *height;
    result.typography = *role;
    result.icon = &icon_iterator->second;
    result.font_resource = std::move(*font);
    result.color = theme_.palette().at((*role)->color);
    return result;
  }

  [[nodiscard]] std::expected<MeasuredNode, LayoutError>
  measure_primitive(const SceneNode &scene, const Spacer &spacer, const std::string &path) const {
    MeasuredNode result{&scene, path};
    result.flexible = spacer.flexible;
    if (!spacer.flexible) {
      auto size = token(theme_.spacers(), spacer.size_token, LayoutErrorCode::unknown_spacer,
                        path + "/size_token");
      if (!size) {
        return std::unexpected(size.error());
      }
      result.width = *size;
      result.height = *size;
      result.minimum_width = *size;
      result.minimum_height = *size;
      result.token_size = *size;
    }
    return result;
  }

  [[nodiscard]] std::expected<MeasuredNode, LayoutError>
  measure_primitive(const SceneNode &scene, const Progress &progress,
                    const std::string &path) const {
    if (!std::isfinite(progress.value) || progress.value < 0.0 || progress.value > 1.0) {
      return std::unexpected(error(LayoutErrorCode::impossible_constraints, path + "/value",
                                   "progress value must be finite and normalized"));
    }
    const auto color = theme_.palette().find(progress.state);
    if (color == theme_.palette().end()) {
      return std::unexpected(
          error(LayoutErrorCode::unknown_role, path + "/state", "unknown progress state role"));
    }
    if (progress.shape == ProgressShape::ring) {
      auto diameter = rounded_pixel(theme_.progress().ring_diameter, path);
      auto thickness = rounded_pixel(theme_.progress().ring_thickness, path);
      if (!diameter) {
        return std::unexpected(diameter.error());
      }
      if (!thickness) {
        return std::unexpected(thickness.error());
      }
      MeasuredNode result{&scene, path};
      result.width = *diameter;
      result.height = *diameter;
      result.minimum_width = *diameter;
      result.minimum_height = *diameter;
      result.track_height = *thickness;
      result.color = color->second;
      return result;
    }
    auto role = typography("body", path + "/label");
    if (!role) {
      return std::unexpected(role.error());
    }
    auto font = font_resource((*role)->font, path + "/label");
    if (!font) {
      return std::unexpected(font.error());
    }
    auto unit = token(theme_.spacers(), "normal", LayoutErrorCode::unknown_spacer, path);
    auto gap = token(theme_.gaps(), "normal", LayoutErrorCode::unknown_gap, path);
    if (!unit) {
      return std::unexpected(unit.error());
    }
    if (!gap) {
      return std::unexpected(gap.error());
    }

    int label_width = 0;
    int label_height = 0;
    if (!progress.label.empty()) {
      auto label = measure_text(*font, **role, progress.label, path + "/label");
      if (!label) {
        return std::unexpected(label.error());
      }
      label_width = label->first;
      label_height = label->second;
    }
    MeasuredNode result{&scene, path};
    auto track_width = checked_multiply(*unit, 8, path);
    if (!track_width) {
      return std::unexpected(track_width.error());
    }
    result.track_width = *track_width;
    auto track_height = rounded_pixel(theme_.progress().linear_thickness, path);
    if (!track_height) {
      return std::unexpected(track_height.error());
    }
    result.track_height = *track_height;
    result.label_width = label_width;
    result.label_height = label_height;
    result.gap = progress.label.empty() ? 0 : *gap;
    result.width = std::max(label_width, result.track_width);
    auto label_and_gap = checked_add(label_height, result.gap, path);
    if (!label_and_gap) {
      return std::unexpected(label_and_gap.error());
    }
    auto height = checked_add(*label_and_gap, result.track_height, path);
    if (!height) {
      return std::unexpected(height.error());
    }
    result.height = *height;
    result.minimum_width = result.width;
    result.minimum_height = result.height;
    result.expands_width = progress.label.empty();
    result.typography = *role;
    result.font_resource = std::move(*font);
    result.color = color->second;
    return result;
  }

  [[nodiscard]] std::expected<MeasuredNode, LayoutError>
  measure_primitive(const SceneNode &scene, const Indicator &indicator,
                    const std::string &path) const {
    const auto color = theme_.palette().find(indicator.state);
    if (color == theme_.palette().end()) {
      return std::unexpected(
          error(LayoutErrorCode::unknown_role, path + "/state", "unknown indicator state role"));
    }
    auto diameter = rounded_pixel(theme_.indicator().diameter, path);
    if (!diameter) {
      return std::unexpected(diameter.error());
    }
    MeasuredNode result{&scene, path};
    result.width = *diameter;
    result.height = *diameter;
    result.minimum_width = *diameter;
    result.minimum_height = *diameter;
    result.color = color->second;
    return result;
  }

  template <typename Container>
  [[nodiscard]] std::expected<MeasuredNode, LayoutError>
  measure_container(const SceneNode &scene, const Container &container, const std::string &path,
                    Axis axis) const {
    auto resolved_alignment = alignment(container.alignment, path + "/alignment");
    if (!resolved_alignment) {
      return std::unexpected(resolved_alignment.error());
    }
    auto resolved_gap =
        token(theme_.gaps(), container.gap, LayoutErrorCode::unknown_gap, path + "/gap");
    if (!resolved_gap) {
      return std::unexpected(resolved_gap.error());
    }

    MeasuredNode result{&scene, path};
    result.alignment = *resolved_alignment;
    result.gap = *resolved_gap;
    result.children.reserve(container.children.size());
    int intrinsic_main = 0;
    int minimum_main = 0;
    int intrinsic_cross = 0;
    int minimum_cross = 0;
    bool expands_width = false;
    bool expands_height = false;
    for (std::size_t index = 0; index < container.children.size(); ++index) {
      auto child = measure(*container.children[index], path + "/children/" + std::to_string(index));
      if (!child) {
        return std::unexpected(child.error());
      }
      expands_width |= axis == Axis::horizontal ? expands_main(*child, axis) : child->expands_width;
      expands_height |= axis == Axis::vertical ? expands_main(*child, axis) : child->expands_height;
      auto next_intrinsic = checked_add(intrinsic_main, main_size(*child, axis, false), path);
      auto next_minimum = checked_add(minimum_main, main_size(*child, axis, true), path);
      if (!next_intrinsic) {
        return std::unexpected(next_intrinsic.error());
      }
      if (!next_minimum) {
        return std::unexpected(next_minimum.error());
      }
      intrinsic_main = *next_intrinsic;
      minimum_main = *next_minimum;
      intrinsic_cross = std::max(intrinsic_cross, cross_size(*child, axis, false));
      minimum_cross = std::max(minimum_cross, cross_size(*child, axis, true));
      result.children.push_back(std::move(*child));
    }
    if (result.children.size() > 1) {
      const auto gap_count = static_cast<int>(result.children.size() - 1);
      auto gap_total = checked_multiply(result.gap, gap_count, path);
      if (!gap_total) {
        return std::unexpected(gap_total.error());
      }
      auto next_intrinsic = checked_add(intrinsic_main, *gap_total, path);
      auto next_minimum = checked_add(minimum_main, *gap_total, path);
      if (!next_intrinsic) {
        return std::unexpected(next_intrinsic.error());
      }
      if (!next_minimum) {
        return std::unexpected(next_minimum.error());
      }
      intrinsic_main = *next_intrinsic;
      minimum_main = *next_minimum;
    }
    if (axis == Axis::horizontal) {
      result.width = intrinsic_main;
      result.height = intrinsic_cross;
      result.minimum_width = minimum_main;
      result.minimum_height = minimum_cross;
    } else {
      result.width = intrinsic_cross;
      result.height = intrinsic_main;
      result.minimum_width = minimum_cross;
      result.minimum_height = minimum_main;
    }
    result.expands_width = expands_width;
    result.expands_height = expands_height;
    return result;
  }

  [[nodiscard]] std::expected<MeasuredNode, LayoutError>
  measure_primitive(const SceneNode &scene, const Row &row, const std::string &path) const {
    return measure_container(scene, row, path, Axis::horizontal);
  }

  [[nodiscard]] std::expected<MeasuredNode, LayoutError>
  measure_primitive(const SceneNode &scene, const Column &column, const std::string &path) const {
    return measure_container(scene, column, path, Axis::vertical);
  }

  [[nodiscard]] std::expected<MeasuredNode, LayoutError>
  measure_primitive(const SceneNode &scene, const Button &button, const std::string &path) const {
    auto padding = token(theme_.spacers(), "normal", LayoutErrorCode::unknown_spacer, path);
    if (!padding) {
      return std::unexpected(padding.error());
    }
    auto child = measure(*button.content, path + "/content");
    if (!child) {
      return std::unexpected(child.error());
    }
    MeasuredNode result{&scene, path};
    result.padding = *padding;
    auto doubled_padding = checked_multiply(2, result.padding, path);
    if (!doubled_padding) {
      return std::unexpected(doubled_padding.error());
    }
    auto width = checked_add(child->width, *doubled_padding, path);
    auto height = checked_add(child->height, *doubled_padding, path);
    auto minimum_width = checked_add(child->minimum_width, *doubled_padding, path);
    auto minimum_height = checked_add(child->minimum_height, *doubled_padding, path);
    for (const auto *value : {&width, &height, &minimum_width, &minimum_height}) {
      if (!value->has_value()) {
        return std::unexpected(value->error());
      }
    }
    result.width = *width;
    result.height = *height;
    result.minimum_width = *minimum_width;
    result.minimum_height = *minimum_height;
    result.expands_width = child->expands_width;
    result.expands_height = child->expands_height;
    result.children.push_back(std::move(*child));
    return result;
  }

  [[nodiscard]] std::expected<MeasuredNode, LayoutError>
  measure_primitive(const SceneNode &scene, const ActionRegion &region,
                    const std::string &path) const {
    auto child = measure(*region.content, path + "/content");
    if (!child) {
      return std::unexpected(child.error());
    }
    MeasuredNode result{&scene, path};
    result.width = child->width;
    result.height = child->height;
    result.minimum_width = child->minimum_width;
    result.minimum_height = child->minimum_height;
    result.expands_width = child->expands_width;
    result.expands_height = child->expands_height;
    result.children.push_back(std::move(*child));
    return result;
  }

  template <typename Primitive>
  [[nodiscard]] static std::expected<void, LayoutError>
  constrain_primitive(MeasuredNode & /*node*/, const Primitive & /*primitive*/,
                      int /*available_width*/) {
    return {};
  }

  [[nodiscard]] std::expected<void, LayoutError>
  constrain_primitive(MeasuredNode &node, const RichText &rich, int available_width) const {
    if (rich_metrics_ == nullptr || available_width <= 0) {
      return std::unexpected(error(LayoutErrorCode::impossible_constraints, node.path,
                                   "rich text has no positive assigned width"));
    }
    auto composition = rich_metrics_->compose(rich, available_width);
    if (!composition) {
      return std::unexpected(rich_error(composition.error(), node.path));
    }
    if (composition->height < 0 || composition->minimum_width < 0) {
      return std::unexpected(error(LayoutErrorCode::impossible_constraints, node.path,
                                   "rich text metrics must be non-negative"));
    }
    node.width = available_width;
    node.height = composition->height;
    node.minimum_width = composition->minimum_width;
    node.minimum_height = composition->height;
    node.rich_composition = std::move(*composition);
    return {};
  }

  [[nodiscard]] std::expected<std::vector<int>, LayoutError>
  horizontal_sizes(const MeasuredNode &node, int available_width) const {
    auto gap_total = checked_multiply(
        node.gap, node.children.size() > 1 ? static_cast<int>(node.children.size() - 1) : 0,
        node.path);
    if (!gap_total || *gap_total > available_width) {
      return std::unexpected(gap_total ? error(LayoutErrorCode::impossible_constraints, node.path,
                                               "container gaps exceed the assigned width")
                                       : gap_total.error());
    }
    std::vector<int> sizes;
    std::vector<std::size_t> flexible;
    sizes.reserve(node.children.size());
    int used = *gap_total;
    for (std::size_t index = 0; index < node.children.size(); ++index) {
      const auto &child = node.children[index];
      sizes.push_back(expands_main(child, Axis::horizontal)
                          ? main_size(child, Axis::horizontal, true)
                          : main_size(child, Axis::horizontal, false));
      if (expands_main(child, Axis::horizontal)) {
        flexible.push_back(index);
      }
      auto next = checked_add(used, sizes.back(), node.path);
      if (!next) {
        return std::unexpected(next.error());
      }
      used = *next;
    }
    if (used > available_width) {
      int deficit = used - available_width;
      for (std::size_t index = 0; index < node.children.size() && deficit > 0; ++index) {
        const int minimum = main_size(node.children[index], Axis::horizontal, true);
        const int reduction = std::min(deficit, sizes[index] - minimum);
        sizes[index] -= reduction;
        deficit -= reduction;
      }
      if (deficit > 0) {
        return std::unexpected(error(LayoutErrorCode::impossible_constraints, node.path,
                                     "rigid children exceed the assigned width"));
      }
      used = available_width;
    }
    if (!flexible.empty()) {
      int remaining = available_width - used;
      const int share = remaining / static_cast<int>(flexible.size());
      int remainder = remaining % static_cast<int>(flexible.size());
      for (const auto index : flexible) {
        sizes[index] += share + (remainder > 0 ? 1 : 0);
        remainder = std::max(0, remainder - 1);
      }
    }
    return sizes;
  }

  [[nodiscard]] std::expected<void, LayoutError>
  constrain_primitive(MeasuredNode &node, const Row & /*row*/, int available_width) const {
    auto sizes = horizontal_sizes(node, available_width);
    if (!sizes) {
      return std::unexpected(sizes.error());
    }
    int height = 0;
    int minimum_height = 0;
    int width =
        node.children.size() > 1 ? node.gap * static_cast<int>(node.children.size() - 1) : 0;
    for (std::size_t index = 0; index < node.children.size(); ++index) {
      auto constrained = constrain_width(node.children[index], (*sizes)[index]);
      if (!constrained) {
        return constrained;
      }
      height = std::max(height, node.children[index].height);
      minimum_height = std::max(minimum_height, node.children[index].minimum_height);
      auto next_width = checked_add(width, (*sizes)[index], node.path);
      if (!next_width) {
        return std::unexpected(next_width.error());
      }
      width = *next_width;
    }
    node.width = width;
    node.height = height;
    node.minimum_height = minimum_height;
    return {};
  }

  [[nodiscard]] std::expected<void, LayoutError>
  constrain_primitive(MeasuredNode &node, const Column & /*column*/, int available_width) const {
    int width = 0;
    int minimum_width = 0;
    int height =
        node.children.size() > 1 ? node.gap * static_cast<int>(node.children.size() - 1) : 0;
    int minimum_height = height;
    for (auto &child : node.children) {
      const int child_width =
          child.expands_width ? available_width : std::min(child.width, available_width);
      auto constrained = constrain_width(child, child_width);
      if (!constrained) {
        return constrained;
      }
      width = std::max(width, child.width);
      minimum_width = std::max(minimum_width, child.minimum_width);
      auto next_height = checked_add(height, child.height, node.path);
      auto next_minimum = checked_add(minimum_height, child.minimum_height, node.path);
      if (!next_height) {
        return std::unexpected(next_height.error());
      }
      if (!next_minimum) {
        return std::unexpected(next_minimum.error());
      }
      height = *next_height;
      minimum_height = *next_minimum;
    }
    node.width = node.expands_width ? available_width : width;
    node.height = height;
    node.minimum_width = minimum_width;
    node.minimum_height = minimum_height;
    return {};
  }

  [[nodiscard]] std::expected<void, LayoutError>
  constrain_primitive(MeasuredNode &node, const Button & /*button*/, int available_width) const {
    auto doubled_padding = checked_multiply(2, node.padding, node.path);
    if (!doubled_padding) {
      return std::unexpected(doubled_padding.error());
    }
    auto inner_width = checked_subtract(available_width, *doubled_padding, node.path);
    if (!inner_width || *inner_width < 0) {
      return std::unexpected(inner_width ? error(LayoutErrorCode::impossible_constraints, node.path,
                                                 "button padding exceeds assigned width")
                                         : inner_width.error());
    }
    auto constrained = constrain_width(node.children.front(), *inner_width);
    if (!constrained) {
      return constrained;
    }
    auto height = checked_add(node.children.front().height, *doubled_padding, node.path);
    if (!height) {
      return std::unexpected(height.error());
    }
    node.width = available_width;
    node.height = *height;
    node.minimum_height = *height;
    return {};
  }

  [[nodiscard]] std::expected<void, LayoutError>
  constrain_primitive(MeasuredNode &node, const ActionRegion & /*region*/,
                      int available_width) const {
    auto constrained = constrain_width(node.children.front(), available_width);
    if (!constrained) {
      return constrained;
    }
    const auto &child = node.children.front();
    node.width = child.width;
    node.height = child.height;
    node.minimum_width = child.minimum_width;
    node.minimum_height = child.minimum_height;
    return {};
  }

  [[nodiscard]] std::expected<std::string, LayoutError>
  truncate_end(const MeasuredNode &node, std::string_view text, int available_width) const {
    const std::string ellipsis{"\xE2\x80\xA6"};
    auto ellipsis_size =
        measure_text(node.font_resource, *node.typography, ellipsis, node.path + "/value");
    if (!ellipsis_size) {
      return std::unexpected(ellipsis_size.error());
    }
    if (ellipsis_size->first > available_width) {
      return std::string{};
    }

    std::vector<std::size_t> ends;
    for (std::size_t index = 0; index < text.size();) {
      const auto byte = static_cast<unsigned char>(text[index]);
      std::size_t length = 1;
      if ((byte & 0xE0U) == 0xC0U) {
        length = 2;
      } else if ((byte & 0xF0U) == 0xE0U) {
        length = 3;
      } else if ((byte & 0xF8U) == 0xF0U) {
        length = 4;
      }
      index = std::min(text.size(), index + length);
      ends.push_back(index);
    }
    while (!ends.empty()) {
      const std::string candidate = std::string{text.substr(0, ends.back())} + ellipsis;
      auto candidate_size =
          measure_text(node.font_resource, *node.typography, candidate, node.path + "/value");
      if (!candidate_size) {
        return std::unexpected(candidate_size.error());
      }
      if (candidate_size->first <= available_width) {
        return candidate;
      }
      ends.pop_back();
    }
    return ellipsis;
  }

  [[nodiscard]] std::expected<void, LayoutError>
  place_primitive(const MeasuredNode &node, const Text &text, Rect assigned, Rect clip,
                  std::vector<ContentDrawCommand> &commands,
                  std::vector<InteractionTarget> & /*interactions*/) const {
    if (assigned.height < node.minimum_height || assigned.width < node.minimum_width) {
      return std::unexpected(error(LayoutErrorCode::impossible_constraints, node.path,
                                   "text cannot fit the assigned bounds"));
    }
    std::string rendered = node.text;
    const bool constrained = node.width > assigned.width;
    if (constrained && text.truncation == "end") {
      auto truncated = truncate_end(node, node.text, assigned.width);
      if (!truncated) {
        return std::unexpected(truncated.error());
      }
      rendered = std::move(*truncated);
    }
    const int width = constrained ? assigned.width : node.width;
    auto y = checked_add(assigned.y, (assigned.height - node.height) / 2, node.path);
    if (!y) {
      return std::unexpected(y.error());
    }
    auto clipped = intersect(clip, assigned, node.path);
    if (!clipped) {
      return std::unexpected(clipped.error());
    }
    commands.emplace_back(TextDrawCommand{Rect{assigned.x, *y, width, node.height}, *clipped,
                                          std::move(rendered), node.font_resource, *node.typography,
                                          node.color});
    return {};
  }

  [[nodiscard]] std::expected<void, LayoutError>
  place_primitive(const MeasuredNode &node, const RichText &rich, Rect assigned, Rect clip,
                  std::vector<ContentDrawCommand> &commands,
                  std::vector<InteractionTarget> &interactions) const {
    if (!node.rich_composition || assigned.width < node.minimum_width ||
        assigned.height < node.height) {
      return std::unexpected(error(LayoutErrorCode::impossible_constraints, node.path,
                                   "rich text cannot fit the assigned bounds"));
    }
    auto y = checked_add(assigned.y, (assigned.height - node.height) / 2, node.path);
    if (!y) {
      return std::unexpected(y.error());
    }
    const Rect bounds{assigned.x, *y, assigned.width, node.height};
    auto clipped = intersect(clip, bounds, node.path);
    if (!clipped) {
      return std::unexpected(clipped.error());
    }
    commands.emplace_back(RichTextDrawCommand{bounds, *clipped, rich, *node.rich_composition,
                                              node.font_resource, *node.typography, node.color,
                                              theme_.palette().at("accent")});
    for (const auto &link : node.rich_composition->links) {
      auto x = checked_add(bounds.x, link.bounds.x, node.path);
      auto link_y = checked_add(bounds.y, link.bounds.y, node.path);
      if (!x) {
        return std::unexpected(x.error());
      }
      if (!link_y) {
        return std::unexpected(link_y.error());
      }
      const Rect link_bounds{*x, *link_y, link.bounds.width, link.bounds.height};
      auto link_clip = intersect(*clipped, link_bounds, node.path);
      if (!link_clip) {
        return std::unexpected(link_clip.error());
      }
      if (link_clip->width > 0 && link_clip->height > 0) {
        interactions.push_back(InteractionTarget{link_bounds, *link_clip, link.action_id, true,
                                                 link.accessible_label, InteractionKind::link});
      }
    }
    return {};
  }

  [[nodiscard]] std::expected<void, LayoutError>
  place_primitive(const MeasuredNode &node, const Icon &icon, Rect assigned, Rect clip,
                  std::vector<ContentDrawCommand> &commands,
                  std::vector<InteractionTarget> & /*interactions*/) const {
    if (assigned.width < node.width || assigned.height < node.height) {
      return std::unexpected(error(LayoutErrorCode::impossible_constraints, node.path,
                                   "icon cannot fit the assigned bounds"));
    }
    auto x = checked_add(assigned.x, (assigned.width - node.width) / 2, node.path);
    auto y = checked_add(assigned.y, (assigned.height - node.height) / 2, node.path);
    if (!x) {
      return std::unexpected(x.error());
    }
    if (!y) {
      return std::unexpected(y.error());
    }
    auto clipped = intersect(clip, assigned, node.path);
    if (!clipped) {
      return std::unexpected(clipped.error());
    }
    commands.emplace_back(IconDrawCommand{Rect{*x, *y, node.width, node.height}, *clipped,
                                          node.font_resource, *node.typography,
                                          node.icon->codepoint, node.color, icon.accessible_label});
    return {};
  }

  [[nodiscard]] static std::expected<void, LayoutError>
  place_primitive(const MeasuredNode &node, const Image &image, Rect assigned, Rect clip,
                  std::vector<ContentDrawCommand> &commands,
                  std::vector<InteractionTarget> & /*interactions*/) {
    if (assigned.width < node.width || assigned.height < node.height) {
      return std::unexpected(error(LayoutErrorCode::impossible_constraints, node.path,
                                   "image cannot fit the assigned bounds"));
    }
    auto y = checked_add(assigned.y, (assigned.height - node.height) / 2, node.path);
    if (!y) {
      return std::unexpected(y.error());
    }
    auto clipped = intersect(clip, assigned, node.path);
    if (!clipped) {
      return std::unexpected(clipped.error());
    }
    commands.emplace_back(ImageDrawCommand{Rect{assigned.x, *y, node.width, node.height}, *clipped,
                                           image.resource_id, *node.image_role,
                                           image.accessible_label});
    return {};
  }

  [[nodiscard]] static std::expected<void, LayoutError>
  place_primitive(const MeasuredNode & /*node*/, const Spacer & /*spacer*/, Rect /*assigned*/,
                  Rect /*clip*/, std::vector<ContentDrawCommand> & /*commands*/,
                  std::vector<InteractionTarget> & /*interactions*/) {
    return {};
  }

  [[nodiscard]] std::expected<void, LayoutError>
  place_primitive(const MeasuredNode &node, const Progress &progress, Rect assigned, Rect clip,
                  std::vector<ContentDrawCommand> &commands,
                  std::vector<InteractionTarget> & /*interactions*/) const {
    if (assigned.width < node.minimum_width || assigned.height < node.minimum_height) {
      return std::unexpected(error(LayoutErrorCode::impossible_constraints, node.path,
                                   "progress cannot fit the assigned bounds"));
    }
    auto y = checked_add(assigned.y, (assigned.height - node.height) / 2, node.path);
    if (!y) {
      return std::unexpected(y.error());
    }
    const int progress_width = progress.shape == ProgressShape::linear && progress.label.empty()
                                   ? assigned.width
                                   : node.width;
    const Rect bounds{assigned.x, *y, progress_width, node.height};
    auto clipped = intersect(clip, bounds, node.path);
    if (!clipped) {
      return std::unexpected(clipped.error());
    }
    if (progress.shape == ProgressShape::ring) {
      auto track_color = resolve_theme_color(theme_, theme_.progress().track);
      if (const auto opacity = theme_.progress().ring_track_opacity) {
        track_color = node.color;
        track_color.alpha = static_cast<std::uint8_t>(
            std::lround(static_cast<double>(track_color.alpha) * *opacity));
      }
      commands.emplace_back(RingProgressDrawCommand{bounds, *clipped, progress.value,
                                                    node.track_height, track_color, node.color,
                                                    node.path, progress.transition_from});
      return {};
    }
    if (!progress.label.empty()) {
      commands.emplace_back(TextDrawCommand{
          Rect{assigned.x, *y, node.label_width, node.label_height}, *clipped, progress.label,
          node.font_resource, *node.typography, theme_.palette().at(node.typography->color)});
    }
    auto label_bottom = checked_add(*y, node.label_height, node.path);
    if (!label_bottom) {
      return std::unexpected(label_bottom.error());
    }
    auto track_y = checked_add(*label_bottom, node.gap, node.path);
    if (!track_y) {
      return std::unexpected(track_y.error());
    }
    const int track_width = progress.label.empty() ? assigned.width : node.track_width;
    const Rect track{assigned.x, *track_y, track_width, node.track_height};
    auto fill_width = rounded_pixel(static_cast<double>(track_width) * progress.value, node.path);
    if (!fill_width) {
      return std::unexpected(fill_width.error());
    }
    commands.emplace_back(ProgressDrawCommand{bounds, *clipped, track,
                                              Rect{track.x, track.y, *fill_width, track.height},
                                              theme_.palette().at("muted"), node.color, node.path,
                                              progress.value, progress.transition_from});
    return {};
  }

  [[nodiscard]] static std::expected<void, LayoutError>
  place_primitive(const MeasuredNode &node, const Indicator &indicator, Rect assigned, Rect clip,
                  std::vector<ContentDrawCommand> &commands,
                  std::vector<InteractionTarget> & /*interactions*/) {
    if (assigned.width < node.width || assigned.height < node.height) {
      return std::unexpected(error(LayoutErrorCode::impossible_constraints, node.path,
                                   "indicator cannot fit the assigned bounds"));
    }
    auto y = checked_add(assigned.y, (assigned.height - node.height) / 2, node.path);
    if (!y) {
      return std::unexpected(y.error());
    }
    const Rect bounds{assigned.x, *y, node.width, node.height};
    auto clipped = intersect(clip, bounds, node.path);
    if (!clipped) {
      return std::unexpected(clipped.error());
    }
    commands.emplace_back(
        IndicatorDrawCommand{bounds, *clipped, node.color, indicator.accessible_label});
    return {};
  }

  template <typename Container>
  [[nodiscard]] std::expected<void, LayoutError>
  place_container(const MeasuredNode &node, const Container & /*container*/, Rect assigned,
                  Rect clip, std::vector<ContentDrawCommand> &commands,
                  std::vector<InteractionTarget> &interactions, Axis axis) const {
    const int available_main = axis == Axis::horizontal ? assigned.width : assigned.height;
    const int available_cross = axis == Axis::horizontal ? assigned.height : assigned.width;
    auto gap_total_result = checked_multiply(
        node.gap, node.children.size() > 1 ? static_cast<int>(node.children.size() - 1) : 0,
        node.path);
    if (!gap_total_result) {
      return std::unexpected(gap_total_result.error());
    }
    const int gap_total = *gap_total_result;
    if (available_main < gap_total) {
      return std::unexpected(error(LayoutErrorCode::impossible_constraints, node.path,
                                   "container gaps exceed the assigned bounds"));
    }

    std::vector<int> sizes;
    sizes.reserve(node.children.size());
    int used = gap_total;
    std::vector<std::size_t> flexible;
    for (std::size_t index = 0; index < node.children.size(); ++index) {
      const auto &child = node.children[index];
      if (expands_main(child, axis)) {
        sizes.push_back(main_size(child, axis, true));
        flexible.push_back(index);
      } else {
        sizes.push_back(main_size(child, axis, false));
      }
      auto next_used = checked_add(used, sizes.back(), node.path);
      if (!next_used) {
        return std::unexpected(next_used.error());
      }
      used = *next_used;
    }

    if (used > available_main) {
      int deficit = used - available_main;
      for (std::size_t index = 0; index < node.children.size() && deficit > 0; ++index) {
        const int minimum = main_size(node.children[index], axis, true);
        const int reduction = std::min(deficit, sizes[index] - minimum);
        sizes[index] -= reduction;
        deficit -= reduction;
      }
      if (deficit > 0) {
        return std::unexpected(error(LayoutErrorCode::impossible_constraints, node.path,
                                     "rigid children exceed the assigned main axis"));
      }
      used = available_main;
    }

    if (!flexible.empty()) {
      int remaining = available_main - used;
      const int share = remaining / static_cast<int>(flexible.size());
      int remainder = remaining % static_cast<int>(flexible.size());
      for (const auto index : flexible) {
        auto expanded = checked_add(sizes[index], share + (remainder > 0 ? 1 : 0), node.path);
        if (!expanded) {
          return std::unexpected(expanded.error());
        }
        sizes[index] = *expanded;
        remainder = std::max(0, remainder - 1);
      }
    }

    int cursor = axis == Axis::horizontal ? assigned.x : assigned.y;
    auto child_clip = intersect(clip, assigned, node.path);
    if (!child_clip) {
      return std::unexpected(child_clip.error());
    }
    for (std::size_t index = 0; index < node.children.size(); ++index) {
      const auto &child = node.children[index];
      int child_cross = cross_size(child, axis, false);
      const bool expands_cross =
          axis == Axis::horizontal ? child.expands_height : child.expands_width;
      if (expands_cross) {
        child_cross = available_cross;
      } else {
        child_cross = std::min(child_cross, available_cross);
      }
      if (cross_size(child, axis, true) > child_cross) {
        return std::unexpected(error(LayoutErrorCode::impossible_constraints, child.path,
                                     "child exceeds the assigned cross axis"));
      }
      int cross_offset = 0;
      if (node.alignment == Alignment::center) {
        cross_offset = (available_cross - child_cross) / 2;
      } else if (node.alignment == Alignment::end) {
        cross_offset = available_cross - child_cross;
      }
      auto cross_position =
          checked_add(axis == Axis::horizontal ? assigned.y : assigned.x, cross_offset, child.path);
      if (!cross_position) {
        return std::unexpected(cross_position.error());
      }
      const Rect child_bounds = axis == Axis::horizontal
                                    ? Rect{cursor, *cross_position, sizes[index], child_cross}
                                    : Rect{*cross_position, cursor, child_cross, sizes[index]};
      auto placed = place(child, child_bounds, *child_clip, commands, interactions);
      if (!placed) {
        return placed;
      }
      auto after_child = checked_add(cursor, sizes[index], node.path);
      if (!after_child) {
        return std::unexpected(after_child.error());
      }
      cursor = *after_child;
      if (index + 1 < node.children.size()) {
        auto after_gap = checked_add(cursor, node.gap, node.path);
        if (!after_gap) {
          return std::unexpected(after_gap.error());
        }
        cursor = *after_gap;
      }
    }
    return {};
  }

  [[nodiscard]] std::expected<void, LayoutError>
  place_primitive(const MeasuredNode &node, const Row &row, Rect assigned, Rect clip,
                  std::vector<ContentDrawCommand> &commands,
                  std::vector<InteractionTarget> &interactions) const {
    return place_container(node, row, assigned, clip, commands, interactions, Axis::horizontal);
  }

  [[nodiscard]] std::expected<void, LayoutError>
  place_primitive(const MeasuredNode &node, const Column &column, Rect assigned, Rect clip,
                  std::vector<ContentDrawCommand> &commands,
                  std::vector<InteractionTarget> &interactions) const {
    return place_container(node, column, assigned, clip, commands, interactions, Axis::vertical);
  }

  [[nodiscard]] std::expected<void, LayoutError>
  place_primitive(const MeasuredNode &node, const Button &button, Rect assigned, Rect clip,
                  std::vector<ContentDrawCommand> &commands,
                  std::vector<InteractionTarget> &interactions) const {
    const int width = std::min(node.width, assigned.width);
    const int height = std::min(node.height, assigned.height);
    if (width < node.minimum_width || height < node.minimum_height) {
      return std::unexpected(error(LayoutErrorCode::impossible_constraints, node.path,
                                   "button cannot fit the assigned bounds"));
    }
    auto y = checked_add(assigned.y, (assigned.height - height) / 2, node.path);
    if (!y) {
      return std::unexpected(y.error());
    }
    const Rect bounds{assigned.x, *y, width, height};
    auto clipped = intersect(clip, bounds, node.path);
    if (!clipped) {
      return std::unexpected(clipped.error());
    }
    const auto &background =
        button.enabled ? theme_.buttons().background : theme_.buttons().disabled_background;
    commands.emplace_back(ButtonDecorationDrawCommand{
        bounds, *clipped, resolve_theme_color(theme_, background), button.enabled});
    interactions.push_back(InteractionTarget{bounds, *clipped, button.action_id, button.enabled,
                                             button.accessible_label, InteractionKind::button});

    const auto &child = node.children.front();
    auto doubled_padding = checked_multiply(2, node.padding, node.path);
    auto inner_x = checked_add(bounds.x, node.padding, node.path);
    auto inner_y = checked_add(bounds.y, node.padding, node.path);
    if (!doubled_padding) {
      return std::unexpected(doubled_padding.error());
    }
    if (!inner_x) {
      return std::unexpected(inner_x.error());
    }
    if (!inner_y) {
      return std::unexpected(inner_y.error());
    }
    auto inner_width = checked_subtract(bounds.width, *doubled_padding, node.path);
    auto inner_height = checked_subtract(bounds.height, *doubled_padding, node.path);
    if (!inner_width) {
      return std::unexpected(inner_width.error());
    }
    if (!inner_height) {
      return std::unexpected(inner_height.error());
    }
    const Rect inner{*inner_x, *inner_y, *inner_width, *inner_height};
    const int child_width = std::min(child.width, inner.width);
    const int child_height = std::min(child.height, inner.height);
    auto child_x = checked_add(inner.x, (inner.width - child_width) / 2, child.path);
    auto child_y = checked_add(inner.y, (inner.height - child_height) / 2, child.path);
    if (!child_x) {
      return std::unexpected(child_x.error());
    }
    if (!child_y) {
      return std::unexpected(child_y.error());
    }
    const Rect child_bounds{*child_x, *child_y, child_width, child_height};
    return place(child, child_bounds, *clipped, commands, interactions);
  }

  [[nodiscard]] std::expected<void, LayoutError>
  place_primitive(const MeasuredNode &node, const ActionRegion &region, Rect assigned, Rect clip,
                  std::vector<ContentDrawCommand> &commands,
                  std::vector<InteractionTarget> &interactions) const {
    if (assigned.width < node.minimum_width || assigned.height < node.minimum_height) {
      return std::unexpected(error(LayoutErrorCode::impossible_constraints, node.path,
                                   "action region cannot fit the assigned bounds"));
    }
    auto clipped = intersect(clip, assigned, node.path);
    if (!clipped) {
      return std::unexpected(clipped.error());
    }
    interactions.push_back(InteractionTarget{assigned, *clipped, region.action_id, region.enabled,
                                             region.accessible_label,
                                             InteractionKind::action_region});
    return place(node.children.front(), assigned, *clipped, commands, interactions);
  }

  const Theme &theme_;
  const GlyphMetrics &metrics_;
  const RichTextMetrics *rich_metrics_;
  ViewMode mode_;
  bool root_leading_cap_;
};

[[nodiscard]] std::expected<const ViewGeometry *, LayoutError>
geometry_for(const Theme &theme, ViewMode mode, std::string_view compact_style) {
  if (mode == ViewMode::expanded) {
    return &theme.views().expanded;
  }
  if (compact_style.empty()) {
    return &theme.views().compact;
  }
  const auto style = theme.views().compact_styles.find(compact_style);
  if (style == theme.views().compact_styles.end()) {
    return std::unexpected(LayoutError{LayoutErrorCode::unknown_compact_style,
                                       "/presentation/compact_style", "unknown compact style"});
  }
  return &style->second;
}

[[nodiscard]] const ImageRole *root_leading_cap_role(const SceneNode &scene, const Theme &theme) {
  const auto *row = std::get_if<Row>(&scene.value);
  if (row == nullptr || row->children.empty()) {
    return nullptr;
  }
  const auto *image = std::get_if<Image>(&row->children.front()->value);
  if (image == nullptr) {
    return nullptr;
  }
  const auto role = theme.images().find(image->role);
  if (role == theme.images().end() || role->second.placement != ImagePlacement::leading_cap) {
    return nullptr;
  }
  return &role->second;
}

} // namespace

RectInsets shadow_insets(const RoundedView &view) {
  if (view.shadow.color.alpha == 0) {
    return {};
  }
  const int radius = view.shadow.blur + view.shadow.spread;
  return RectInsets{
      std::max(0, radius - view.shadow.offset_x),
      std::max(0, radius - view.shadow.offset_y),
      std::max(0, radius + view.shadow.offset_x),
      std::max(0, radius + view.shadow.offset_y),
  };
}

[[nodiscard]] static std::expected<LayoutPlan, LayoutError>
layout_scene_impl(const SceneNode &scene, const Theme &theme, ViewMode mode,
                  const GlyphMetrics &glyph_metrics, const RichTextMetrics *rich_text_metrics,
                  std::string_view compact_style) {
  const auto validation = validate_scene(scene);
  if (!validation) {
    return std::unexpected(error(LayoutErrorCode::impossible_constraints, validation.error().path,
                                 "scene is invalid"));
  }

  const ImageRole *leading_cap = root_leading_cap_role(scene, theme);
  const LayoutBuilder builder{theme, glyph_metrics, rich_text_metrics, mode,
                              leading_cap != nullptr};
  auto measured = builder.measure(scene, "");
  if (!measured) {
    return std::unexpected(measured.error());
  }

  const auto geometry = geometry_for(theme, mode, compact_style);
  if (!geometry) {
    return std::unexpected(geometry.error());
  }
  auto horizontal_padding = rounded_pixel((*geometry)->padding_horizontal, "");
  auto vertical_padding = rounded_pixel((*geometry)->padding_vertical, "");
  auto minimum_width = rounded_pixel((*geometry)->min_width, "");
  auto maximum_width = rounded_pixel((*geometry)->max_width, "");
  auto minimum_height = rounded_pixel((*geometry)->min_height, "");
  auto maximum_height = rounded_pixel((*geometry)->max_height, "");
  auto radius = rounded_pixel((*geometry)->radius, "");
  auto border = rounded_pixel((*geometry)->border, "");
  auto shadow_offset_x = rounded_signed_pixel(theme.shadow().offset_x, "shadow.offset_x");
  auto shadow_offset_y = rounded_signed_pixel(theme.shadow().offset_y, "shadow.offset_y");
  auto shadow_blur = rounded_pixel(theme.shadow().blur, "shadow.blur");
  auto shadow_spread = rounded_pixel(theme.shadow().spread, "shadow.spread");
  for (const auto *value : {&horizontal_padding, &vertical_padding, &minimum_width, &maximum_width,
                            &minimum_height, &maximum_height, &radius, &border, &shadow_offset_x,
                            &shadow_offset_y, &shadow_blur, &shadow_spread}) {
    if (!value->has_value()) {
      return std::unexpected(value->error());
    }
  }

  if (mode == ViewMode::compact && compact_style.empty() && contains_ring_progress(scene)) {
    auto compact_height = rounded_pixel(theme.progress().compact_height, "progress.compact_height");
    if (!compact_height) {
      return std::unexpected(compact_height.error());
    }
    const int extra_height = std::max(0, *compact_height - measured->height);
    *vertical_padding = std::max(*vertical_padding, (extra_height + 1) / 2);
    *minimum_height = std::max(*minimum_height, *compact_height);
    *maximum_height = std::max(*maximum_height, *compact_height);
    *radius = std::max(*radius, *compact_height / 2);
  }

  int leading_inset = *horizontal_padding;
  if (leading_cap != nullptr) {
    auto image_width = rounded_pixel(leading_cap->width, "/children/0/role");
    auto image_height = rounded_pixel(leading_cap->height, "/children/0/role");
    if (!image_width) {
      return std::unexpected(image_width.error());
    }
    if (!image_height) {
      return std::unexpected(image_height.error());
    }
    if (mode != ViewMode::compact || *minimum_height != *maximum_height ||
        *radius * 2 != *minimum_height || *image_width != *image_height ||
        *image_height >= *minimum_height) {
      return std::unexpected(error(
          LayoutErrorCode::invalid_image_placement, "/children/0/role",
          "leading-cap image requires a fixed circular compact cap and a smaller square image"));
    }
    leading_inset = (*minimum_height - *image_width) / 2;
  }

  auto horizontal_insets = checked_add(leading_inset, *horizontal_padding, "");
  auto doubled_vertical_padding = checked_multiply(2, *vertical_padding, "");
  if (!horizontal_insets) {
    return std::unexpected(horizontal_insets.error());
  }
  if (!doubled_vertical_padding) {
    return std::unexpected(doubled_vertical_padding.error());
  }
  auto maximum_content_width = checked_subtract(*maximum_width, *horizontal_insets, "");
  auto maximum_content_height = checked_subtract(*maximum_height, *doubled_vertical_padding, "");
  if (!maximum_content_width) {
    return std::unexpected(maximum_content_width.error());
  }
  if (!maximum_content_height) {
    return std::unexpected(maximum_content_height.error());
  }
  if (*maximum_content_width <= 0 || *maximum_content_height <= 0) {
    return std::unexpected(error(LayoutErrorCode::impossible_constraints, "",
                                 "view has no positive content bounds after rounding"));
  }
  if (*maximum_content_width < measured->minimum_width ||
      *maximum_content_height < measured->minimum_height) {
    return std::unexpected(error(LayoutErrorCode::impossible_constraints, "",
                                 "scene minimum size exceeds the view maximum"));
  }

  auto desired_width = checked_add(measured->width, *horizontal_insets, "");
  if (!desired_width) {
    return std::unexpected(desired_width.error());
  }
  const int view_width = std::clamp(*desired_width, *minimum_width, *maximum_width);
  auto content_width = checked_subtract(view_width, *horizontal_insets, "");
  if (!content_width) {
    return std::unexpected(content_width.error());
  }
  auto constrained = builder.constrain_width(*measured, *content_width);
  if (!constrained) {
    return std::unexpected(constrained.error());
  }
  if (measured->height > *maximum_content_height) {
    return std::unexpected(error(LayoutErrorCode::impossible_constraints, "",
                                 "width-constrained scene height exceeds the view maximum"));
  }
  auto desired_height = checked_add(measured->height, *doubled_vertical_padding, "");
  if (!desired_height) {
    return std::unexpected(desired_height.error());
  }
  const int view_height = std::clamp(*desired_height, *minimum_height, *maximum_height);
  const Rect view_bounds{0, 0, view_width, view_height};
  auto content_height = checked_subtract(view_height, *doubled_vertical_padding, "");
  if (!content_height) {
    return std::unexpected(content_height.error());
  }
  if (*content_width <= 0 || *content_height <= 0) {
    return std::unexpected(error(LayoutErrorCode::impossible_constraints, "",
                                 "view has no positive content bounds after rounding"));
  }
  const Rect content_bounds{leading_inset, *vertical_padding, *content_width, *content_height};
  LayoutPlan plan{
      RoundedView{view_bounds, std::min(*radius, std::min(view_width, view_height) / 2), *border,
                  theme.palette().at("surface"), theme.palette().at("muted"),
                  ViewShadow{*shadow_offset_x, *shadow_offset_y, *shadow_blur, *shadow_spread,
                             resolve_theme_color(theme, theme.shadow().color)}},
      {},
      {}};
  auto placed =
      builder.place(*measured, content_bounds, content_bounds, plan.content, plan.interactions);
  if (!placed) {
    return std::unexpected(placed.error());
  }
  if (leading_cap != nullptr && !plan.content.empty()) {
    if (auto *image = std::get_if<ImageDrawCommand>(&plan.content.front()); image != nullptr) {
      image->clip = view_bounds;
    }
  }
  return plan;
}

std::expected<LayoutPlan, LayoutError> layout_scene(const SceneNode &scene, const Theme &theme,
                                                    ViewMode mode,
                                                    const GlyphMetrics &glyph_metrics,
                                                    std::string_view compact_style) {
  return layout_scene_impl(scene, theme, mode, glyph_metrics, nullptr, compact_style);
}

std::expected<LayoutPlan, LayoutError> layout_scene(const SceneNode &scene, const Theme &theme,
                                                    ViewMode mode,
                                                    const GlyphMetrics &glyph_metrics,
                                                    const RichTextMetrics &rich_text_metrics,
                                                    std::string_view compact_style) {
  return layout_scene_impl(scene, theme, mode, glyph_metrics, &rich_text_metrics, compact_style);
}

} // namespace gisland
