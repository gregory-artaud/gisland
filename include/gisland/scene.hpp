#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace gisland {

struct SceneNode;
using SceneChild = std::shared_ptr<const SceneNode>;

struct Text {
  std::string value;
  std::string role;
  std::string truncation{"end"};
};

struct Icon {
  std::string name;
  std::string accessible_label;
  std::string role{"body"};
};

struct Image {
  std::string resource_id;
  std::string role;
  std::string accessible_label;
};

enum class ImageFormat { rgba8 };

struct ImageResource {
  std::string id;
  ImageFormat format;
  std::uint32_t width;
  std::uint32_t height;
  std::shared_ptr<const std::vector<std::uint8_t>> pixels;
};

enum class TextEmphasis { bold, italic, underline };

struct RichTextSpan {
  std::string value;
  std::vector<TextEmphasis> emphasis;

  bool operator==(const RichTextSpan &) const = default;
};

struct RichLinkSpan {
  std::string value;
  std::vector<TextEmphasis> emphasis;
  std::string action_id;
  std::string accessible_label;

  bool operator==(const RichLinkSpan &) const = default;
};

struct RichInlineImage {
  std::string resource_id;
  std::string role;
  std::string accessible_label;

  bool operator==(const RichInlineImage &) const = default;
};

using RichContent = std::variant<RichTextSpan, RichLinkSpan, RichInlineImage>;

struct RichText {
  std::string role;
  std::vector<RichContent> content;

  bool operator==(const RichText &) const = default;
};

struct Spacer {
  bool flexible{true};
  std::string size_token;
};

enum class ProgressShape { linear, ring };

struct Progress {
  Progress(double initial_value, std::string initial_label, std::string initial_state,
           ProgressShape initial_shape = ProgressShape::linear,
           std::optional<double> initial_transition_from = std::nullopt)
      : value(initial_value), label(std::move(initial_label)), state(std::move(initial_state)),
        shape(initial_shape), transition_from(initial_transition_from) {}

  double value;
  std::string label;
  std::string state;
  ProgressShape shape{ProgressShape::linear};
  std::optional<double> transition_from;
};

struct Indicator {
  std::string state;
  std::string accessible_label;
};

struct Row {
  explicit Row(std::vector<SceneNode> nodes, std::string alignment = "center",
               std::string gap = "normal");

  std::vector<SceneChild> children;
  std::string alignment;
  std::string gap;
};

struct Column {
  explicit Column(std::vector<SceneNode> nodes, std::string alignment = "center",
                  std::string gap = "normal");

  std::vector<SceneChild> children;
  std::string alignment;
  std::string gap;
};

struct Button {
  Button(SceneNode content, std::string action_id, bool enabled = true,
         std::string accessible_label = {});

  SceneChild content;
  std::string action_id;
  bool enabled;
  std::string accessible_label;
};

struct ActionRegion {
  ActionRegion(SceneNode content, std::string action_id, bool enabled = true,
               std::string accessible_label = {});

  SceneChild content;
  std::string action_id;
  bool enabled;
  std::string accessible_label;
};

struct SceneNode {
  using Value = std::variant<Text, Icon, Image, RichText, Row, Column, Spacer, Progress, Indicator,
                             Button, ActionRegion>;

  Value value;
};

enum class SceneErrorCode {
  too_deep,
  too_many_nodes,
  text_too_long,
  identifier_too_long,
  invalid_progress,
  empty_action,
  invalid_emphasis
};

struct SceneError {
  SceneErrorCode code;
  std::string path;
};

using SceneValidation = std::expected<void, SceneError>;

[[nodiscard]] SceneValidation validate_scene(const SceneNode &root);

} // namespace gisland
