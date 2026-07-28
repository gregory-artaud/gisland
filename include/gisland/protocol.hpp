#pragma once

#include "gisland/scene.hpp"

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace gisland {

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

} // namespace gisland
