#pragma once

#include <expected>
#include <memory>
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
};

struct Spacer {
  bool flexible{true};
  std::string size_token;
};

struct Progress {
  double value{};
  std::string label;
  std::string state;
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

struct SceneNode {
  using Value = std::variant<Text, Icon, Row, Column, Spacer, Progress, Button>;

  Value value;
};

enum class SceneErrorCode {
  too_deep,
  too_many_nodes,
  text_too_long,
  identifier_too_long,
  invalid_progress,
  empty_action
};

struct SceneError {
  SceneErrorCode code;
  std::string path;
};

using SceneValidation = std::expected<void, SceneError>;

[[nodiscard]] SceneValidation validate_scene(const SceneNode &root);

} // namespace gisland
