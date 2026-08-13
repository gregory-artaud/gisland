#pragma once

#include "gisland/control.hpp"

#include <chrono>
#include <expected>
#include <string>
#include <string_view>

namespace gisland {

struct ControlClientError {
  std::string message;
};

[[nodiscard]] std::chrono::milliseconds control_read_timeout(const ControlCommand &command);

[[nodiscard]] std::expected<ControlResponse, ControlClientError>
send_control_command(std::string_view socket_path, const ControlCommand &command,
                     std::chrono::milliseconds phase_timeout = std::chrono::seconds{2});
[[nodiscard]] std::string format_control_output(const ControlResponse &response, bool json_output);

} // namespace gisland
