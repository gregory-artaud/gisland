#pragma once

#include "gisland/scene.hpp"

#include <nlohmann/json.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace gisland {

struct DataBinding {
  std::string path;
};

template <typename T> using TemplateValue = std::variant<T, DataBinding>;

struct TemplateText {
  TemplateValue<std::string> value;
  TemplateValue<std::string> role;
  TemplateValue<std::string> truncation{std::string{"end"}};
};

struct TemplateIcon {
  TemplateValue<std::string> name;
  TemplateValue<std::string> accessible_label{std::string{}};
};

struct TemplateSpacer {
  TemplateValue<bool> flexible{true};
  TemplateValue<std::string> size_token{std::string{}};
};

struct TemplateProgress {
  TemplateValue<double> value;
  TemplateValue<std::string> label{std::string{}};
  TemplateValue<std::string> state{std::string{}};
  TemplateValue<std::string> shape{std::string{"linear"}};
};

struct TemplateIndicator {
  TemplateValue<std::string> state;
  TemplateValue<std::string> accessible_label;
};

struct SceneTemplate;
using SceneTemplatePtr = std::shared_ptr<const SceneTemplate>;

struct TemplateRepeat {
  DataBinding source;
  std::string alias;
  SceneTemplatePtr body;
};

using TemplateChild = std::variant<SceneTemplatePtr, TemplateRepeat>;

struct TemplateRow {
  std::vector<TemplateChild> children;
  TemplateValue<std::string> alignment{std::string{"center"}};
  TemplateValue<std::string> gap{std::string{"normal"}};
};

struct TemplateColumn {
  std::vector<TemplateChild> children;
  TemplateValue<std::string> alignment{std::string{"center"}};
  TemplateValue<std::string> gap{std::string{"normal"}};
};

struct TemplateButton {
  SceneTemplatePtr content;
  std::string action_id;
  TemplateValue<bool> enabled{true};
  TemplateValue<std::string> accessible_label{std::string{}};
};

struct SceneTemplate {
  using Value = std::variant<TemplateText, TemplateIcon, TemplateRow, TemplateColumn,
                             TemplateSpacer, TemplateProgress, TemplateIndicator, TemplateButton>;
  Value value;
};

struct ModuleViewConfig {
  std::optional<SceneTemplate> compact;
  std::optional<SceneTemplate> expanded;
};

enum class TemplateErrorCode {
  invalid_binding,
  missing_data,
  wrong_type,
  repeat_source_mismatch,
  invalid_template,
  invalid_scene
};

struct TemplateError {
  TemplateErrorCode code;
  std::string template_path;
  std::string data_path;
};

[[nodiscard]] std::expected<SceneNode, TemplateError>
instantiate_template(const SceneTemplate &scene_template, const nlohmann::json &snapshot);

struct InstantiatedViews {
  SceneNode compact;
  std::optional<SceneNode> expanded;
};

class ModuleViewState {
public:
  ModuleViewState(SceneTemplate compact, std::optional<SceneTemplate> expanded = std::nullopt);

  [[nodiscard]] std::expected<void, TemplateError> apply(nlohmann::json snapshot);
  [[nodiscard]] const std::optional<nlohmann::json> &snapshot() const;
  [[nodiscard]] const std::optional<InstantiatedViews> &views() const;

private:
  SceneTemplate compact_template_;
  std::optional<SceneTemplate> expanded_template_;
  std::optional<nlohmann::json> snapshot_;
  std::optional<InstantiatedViews> views_;
};

} // namespace gisland
