#pragma once

#include "gisland/scene.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace gisland {

struct ProtocolVersion {
  int major;
  int minor;

  auto operator<=>(const ProtocolVersion &) const = default;
};

struct InitMessage {
  ProtocolVersion minimum;
  ProtocolVersion maximum;
  std::string instance_id;
  std::vector<std::string> capabilities;
  nlohmann::json configuration;
  std::string locale;
  std::string timezone;
};

struct ActionMessage {
  std::string action_id;
  std::optional<nlohmann::json> value;
};

enum class Visibility { hidden, compact_active, expanded_active };

struct VisibilityMessage {
  Visibility value;
};

struct ShutdownMessage {
  std::string reason;
  std::chrono::milliseconds deadline;
};

using CoreMessage = std::variant<InitMessage, ActionMessage, VisibilityMessage, ShutdownMessage>;

struct PublishMessage {
  std::string context_id;
  int priority;
  std::optional<std::chrono::milliseconds> expires_in;
  SceneNode compact;
  std::optional<SceneNode> expanded;
};

struct DismissMessage {
  std::string context_id;
};

struct ReadyMessage {
  int protocol_major;
  int protocol_minor;
};

using ModuleMessage = std::variant<ReadyMessage, PublishMessage, DismissMessage>;

struct ProtocolError {
  std::string path;
  std::string message;
};

[[nodiscard]] std::expected<ModuleMessage, ProtocolError>
parse_module_message(std::string_view line);

[[nodiscard]] std::string serialize_core_message(const CoreMessage &message);

} // namespace gisland
