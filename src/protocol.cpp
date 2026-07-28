#include "gisland/protocol.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gisland {
namespace {

using Json = nlohmann::json;

[[nodiscard]] ProtocolError error_at(std::string path, std::string message) {
  return ProtocolError{std::move(path), std::move(message)};
}

// The two arguments intentionally share a type: one is the existing JSON pointer and the other
// is the statically selected child field.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] std::string field_path(std::string_view parent, std::string_view field) {
  std::string path{parent};
  path += '/';
  path += field;
  return path;
}

[[nodiscard]] std::expected<const Json *, ProtocolError>
required_field(const Json &object, std::string_view key, std::string_view parent_path) {
  if (!object.is_object()) {
    return std::unexpected(error_at(std::string{parent_path}, "expected an object"));
  }

  const auto iterator = object.find(std::string{key});
  if (iterator == object.end()) {
    return std::unexpected(error_at(field_path(parent_path, key), "missing required field"));
  }
  return &*iterator;
}

[[nodiscard]] std::expected<std::string, ProtocolError>
required_string(const Json &object, std::string_view key, std::string_view parent_path) {
  auto field = required_field(object, key, parent_path);
  if (!field.has_value()) {
    return std::unexpected(field.error());
  }
  if (!(*field)->is_string()) {
    return std::unexpected(error_at(field_path(parent_path, key), "expected a string"));
  }
  return (*field)->get<std::string>();
}

[[nodiscard]] std::expected<std::string, ProtocolError>
optional_string(const Json &object, std::string_view key, std::string default_value,
                std::string_view parent_path) {
  const auto iterator = object.find(std::string{key});
  if (iterator == object.end()) {
    return default_value;
  }
  if (!iterator->is_string()) {
    return std::unexpected(error_at(field_path(parent_path, key), "expected a string"));
  }
  return iterator->get<std::string>();
}

template <typename Integer>
[[nodiscard]] std::expected<Integer, ProtocolError>
required_integer(const Json &object, std::string_view key, std::string_view parent_path) {
  auto field = required_field(object, key, parent_path);
  if (!field.has_value()) {
    return std::unexpected(field.error());
  }
  if (!(*field)->is_number_integer() && !(*field)->is_number_unsigned()) {
    return std::unexpected(error_at(field_path(parent_path, key), "expected an integer"));
  }
  try {
    return (*field)->get<Integer>();
  } catch (const Json::exception &) {
    return std::unexpected(error_at(field_path(parent_path, key), "integer is out of range"));
  }
}

[[nodiscard]] std::expected<double, ProtocolError>
required_number(const Json &object, std::string_view key, std::string_view parent_path) {
  auto field = required_field(object, key, parent_path);
  if (!field.has_value()) {
    return std::unexpected(field.error());
  }
  if (!(*field)->is_number()) {
    return std::unexpected(error_at(field_path(parent_path, key), "expected a number"));
  }
  try {
    return (*field)->get<double>();
  } catch (const Json::exception &) {
    return std::unexpected(error_at(field_path(parent_path, key), "number is out of range"));
  }
}

[[nodiscard]] std::expected<bool, ProtocolError> optional_bool(const Json &object,
                                                               std::string_view key,
                                                               bool default_value,
                                                               std::string_view parent_path) {
  const auto iterator = object.find(std::string{key});
  if (iterator == object.end()) {
    return default_value;
  }
  if (!iterator->is_boolean()) {
    return std::unexpected(error_at(field_path(parent_path, key), "expected a boolean"));
  }
  return iterator->get<bool>();
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_scene(const Json &object,
                                                                  const std::string &path);

[[nodiscard]] std::expected<std::vector<SceneNode>, ProtocolError>
parse_children(const Json &object, const std::string &path) {
  auto field = required_field(object, "children", path);
  if (!field.has_value()) {
    return std::unexpected(field.error());
  }
  if (!(*field)->is_array()) {
    return std::unexpected(error_at(path + "/children", "expected an array"));
  }

  std::vector<SceneNode> children;
  children.reserve((*field)->size());
  for (std::size_t index = 0; index < (*field)->size(); ++index) {
    auto child = parse_scene((*field)->at(index), path + "/children/" + std::to_string(index));
    if (!child.has_value()) {
      return std::unexpected(child.error());
    }
    children.push_back(std::move(*child));
  }
  return children;
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_text(const Json &object,
                                                                 const std::string &path) {
  auto value = required_string(object, "value", path);
  auto role = required_string(object, "role", path);
  auto truncation = optional_string(object, "truncation", "end", path);
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  if (!role.has_value()) {
    return std::unexpected(role.error());
  }
  if (!truncation.has_value()) {
    return std::unexpected(truncation.error());
  }
  return SceneNode{Text{std::move(*value), std::move(*role), std::move(*truncation)}};
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_icon(const Json &object,
                                                                 const std::string &path) {
  auto name = required_string(object, "name", path);
  auto accessible_label = required_string(object, "accessible_label", path);
  if (!name.has_value()) {
    return std::unexpected(name.error());
  }
  if (!accessible_label.has_value()) {
    return std::unexpected(accessible_label.error());
  }
  return SceneNode{Icon{std::move(*name), std::move(*accessible_label)}};
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_spacer(const Json &object,
                                                                   const std::string &path) {
  auto flexible = optional_bool(object, "flexible", true, path);
  auto size_token = optional_string(object, "size_token", "", path);
  if (!flexible.has_value()) {
    return std::unexpected(flexible.error());
  }
  if (!size_token.has_value()) {
    return std::unexpected(size_token.error());
  }
  return SceneNode{Spacer{*flexible, std::move(*size_token)}};
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_progress(const Json &object,
                                                                     const std::string &path) {
  auto value = required_number(object, "value", path);
  auto label = optional_string(object, "label", "", path);
  auto state = optional_string(object, "state", "normal", path);
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  if (!label.has_value()) {
    return std::unexpected(label.error());
  }
  if (!state.has_value()) {
    return std::unexpected(state.error());
  }
  return SceneNode{Progress{*value, std::move(*label), std::move(*state)}};
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_row(const Json &object,
                                                                const std::string &path) {
  auto children = parse_children(object, path);
  auto alignment = optional_string(object, "alignment", "center", path);
  auto gap = optional_string(object, "gap", "normal", path);
  if (!children.has_value()) {
    return std::unexpected(children.error());
  }
  if (!alignment.has_value()) {
    return std::unexpected(alignment.error());
  }
  if (!gap.has_value()) {
    return std::unexpected(gap.error());
  }
  return SceneNode{Row{std::move(*children), std::move(*alignment), std::move(*gap)}};
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_column(const Json &object,
                                                                   const std::string &path) {
  auto children = parse_children(object, path);
  auto alignment = optional_string(object, "alignment", "center", path);
  auto gap = optional_string(object, "gap", "normal", path);
  if (!children.has_value()) {
    return std::unexpected(children.error());
  }
  if (!alignment.has_value()) {
    return std::unexpected(alignment.error());
  }
  if (!gap.has_value()) {
    return std::unexpected(gap.error());
  }
  return SceneNode{Column{std::move(*children), std::move(*alignment), std::move(*gap)}};
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_button(const Json &object,
                                                                   const std::string &path) {
  auto content_field = required_field(object, "content", path);
  if (!content_field.has_value()) {
    return std::unexpected(content_field.error());
  }
  auto content = parse_scene(**content_field, path + "/content");
  auto action_id = required_string(object, "action_id", path);
  auto enabled = optional_bool(object, "enabled", true, path);
  auto accessible_label = required_string(object, "accessible_label", path);
  if (!content.has_value()) {
    return std::unexpected(content.error());
  }
  if (!action_id.has_value()) {
    return std::unexpected(action_id.error());
  }
  if (!enabled.has_value()) {
    return std::unexpected(enabled.error());
  }
  if (!accessible_label.has_value()) {
    return std::unexpected(accessible_label.error());
  }
  return SceneNode{
      Button{std::move(*content), std::move(*action_id), *enabled, std::move(*accessible_label)}};
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_scene(const Json &object,
                                                                  const std::string &path) {
  if (!object.is_object()) {
    return std::unexpected(error_at(path, "expected a scene object"));
  }

  auto type = required_string(object, "type", path);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  if (*type == "text") {
    return parse_text(object, path);
  }
  if (*type == "icon") {
    return parse_icon(object, path);
  }
  if (*type == "row") {
    return parse_row(object, path);
  }
  if (*type == "column") {
    return parse_column(object, path);
  }
  if (*type == "spacer") {
    return parse_spacer(object, path);
  }
  if (*type == "progress") {
    return parse_progress(object, path);
  }
  if (*type == "button") {
    return parse_button(object, path);
  }
  return std::unexpected(error_at(path + "/type", "unknown scene primitive"));
}

[[nodiscard]] std::string scene_error_message(SceneErrorCode code) {
  switch (code) {
  case SceneErrorCode::too_deep:
    return "scene exceeds maximum depth";
  case SceneErrorCode::too_many_nodes:
    return "scene exceeds maximum node count";
  case SceneErrorCode::text_too_long:
    return "text exceeds maximum byte count";
  case SceneErrorCode::invalid_progress:
    return "progress value must be between 0 and 1";
  case SceneErrorCode::empty_action:
    return "button action ID must not be empty";
  }
  return "invalid scene";
}

[[nodiscard]] std::optional<ProtocolError> scene_validation_error(const SceneNode &scene,
                                                                  const std::string &path) {
  const auto validation = validate_scene(scene);
  if (validation.has_value()) {
    return std::nullopt;
  }
  return error_at(path + validation.error().path, scene_error_message(validation.error().code));
}

[[nodiscard]] std::expected<ModuleMessage, ProtocolError> parse_ready(const Json &object) {
  auto protocol_major = required_integer<int>(object, "protocol_major", "");
  auto protocol_minor = required_integer<int>(object, "protocol_minor", "");
  if (!protocol_major.has_value()) {
    return std::unexpected(protocol_major.error());
  }
  if (!protocol_minor.has_value()) {
    return std::unexpected(protocol_minor.error());
  }
  if (*protocol_major < 0) {
    return std::unexpected(error_at("/protocol_major", "protocol version must be non-negative"));
  }
  if (*protocol_minor < 0) {
    return std::unexpected(error_at("/protocol_minor", "protocol version must be non-negative"));
  }
  return ModuleMessage{ReadyMessage{*protocol_major, *protocol_minor}};
}

[[nodiscard]] std::expected<ModuleMessage, ProtocolError> parse_dismiss(const Json &object) {
  auto context_id = required_string(object, "context_id", "");
  if (!context_id.has_value()) {
    return std::unexpected(context_id.error());
  }
  if (context_id->empty()) {
    return std::unexpected(error_at("/context_id", "context ID must not be empty"));
  }
  return ModuleMessage{DismissMessage{std::move(*context_id)}};
}

[[nodiscard]] std::expected<ModuleMessage, ProtocolError> parse_publish(const Json &object) {
  auto context_id = required_string(object, "context_id", "");
  auto priority = required_integer<int>(object, "priority", "");
  auto compact_field = required_field(object, "compact", "");
  if (!context_id.has_value()) {
    return std::unexpected(context_id.error());
  }
  if (context_id->empty()) {
    return std::unexpected(error_at("/context_id", "context ID must not be empty"));
  }
  if (!priority.has_value()) {
    return std::unexpected(priority.error());
  }
  if (!compact_field.has_value()) {
    return std::unexpected(compact_field.error());
  }

  std::optional<std::chrono::milliseconds> expires_in;
  if (object.contains("expires_in_ms")) {
    auto milliseconds = required_integer<std::int64_t>(object, "expires_in_ms", "");
    if (!milliseconds.has_value()) {
      return std::unexpected(milliseconds.error());
    }
    if (*milliseconds < 0) {
      return std::unexpected(error_at("/expires_in_ms", "expiration must be non-negative"));
    }
    expires_in = std::chrono::milliseconds{*milliseconds};
  }

  auto compact = parse_scene(**compact_field, "/compact");
  if (!compact.has_value()) {
    return std::unexpected(compact.error());
  }
  if (auto validation_error = scene_validation_error(*compact, "/compact");
      validation_error.has_value()) {
    return std::unexpected(std::move(*validation_error));
  }

  std::optional<SceneNode> expanded;
  const auto expanded_iterator = object.find("expanded");
  if (expanded_iterator != object.end() && !expanded_iterator->is_null()) {
    auto parsed_expanded = parse_scene(*expanded_iterator, "/expanded");
    if (!parsed_expanded.has_value()) {
      return std::unexpected(parsed_expanded.error());
    }
    if (auto validation_error = scene_validation_error(*parsed_expanded, "/expanded");
        validation_error.has_value()) {
      return std::unexpected(std::move(*validation_error));
    }
    expanded = std::move(*parsed_expanded);
  }

  return ModuleMessage{PublishMessage{std::move(*context_id), *priority, expires_in,
                                      std::move(*compact), std::move(expanded)}};
}

} // namespace

std::expected<ModuleMessage, ProtocolError> parse_module_message(std::string_view line) {
  Json object;
  try {
    object = Json::parse(line);
  } catch (const Json::parse_error &) {
    return std::unexpected(error_at("", "invalid JSON"));
  }

  if (!object.is_object()) {
    return std::unexpected(error_at("", "message must be a JSON object"));
  }

  auto type = required_string(object, "type", "");
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  if (*type == "ready") {
    return parse_ready(object);
  }
  if (*type == "publish") {
    return parse_publish(object);
  }
  if (*type == "dismiss") {
    return parse_dismiss(object);
  }
  return std::unexpected(error_at("/type", "unknown message type"));
}

} // namespace gisland
