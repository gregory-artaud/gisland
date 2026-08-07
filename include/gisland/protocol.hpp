#pragma once

#include "gisland/context.hpp"

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
  std::optional<std::chrono::milliseconds> expires_in{};
  std::optional<SceneNode> compact{};
  std::optional<SceneNode> expanded{};
  std::vector<ImageResource> resources{};
  std::optional<PresentationIntent> presentation{};
  bool independent_views{false};
};

struct DismissMessage {
  std::string context_id;
};

struct ReadyMessage {
  int protocol_major;
  int protocol_minor;
  std::vector<std::string> capabilities;
};

struct DataMessage {
  nlohmann::json value;
};

struct ActionResultMessage {
  std::string action_id;
  bool accepted;
  std::optional<std::string> message;
};

enum class LogLevel { debug, info, warning, error };

struct LogMessage {
  LogLevel level;
  std::string message;
};

using ModuleMessage = std::variant<ReadyMessage, PublishMessage, DismissMessage,
                                   ActionResultMessage, LogMessage, DataMessage>;

struct ProtocolError {
  std::string path;
  std::string message;
};

[[nodiscard]] std::expected<ModuleMessage, ProtocolError>
parse_module_message(std::string_view line);

[[nodiscard]] std::string serialize_core_message(const CoreMessage &message);

} // namespace gisland
