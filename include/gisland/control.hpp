#pragma once

#include "gisland/island.hpp"

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace gisland {

struct OpenControl {
  bool operator==(const OpenControl &) const = default;
};
struct CloseControl {
  bool operator==(const CloseControl &) const = default;
};
struct ToggleControl {
  bool operator==(const ToggleControl &) const = default;
};
struct StatusControl {
  bool operator==(const StatusControl &) const = default;
};
struct ModulesControl {
  bool operator==(const ModulesControl &) const = default;
};
struct ReloadControl {
  bool operator==(const ReloadControl &) const = default;
};
struct RestartModuleControl {
  std::string instance_id;
  bool operator==(const RestartModuleControl &) const = default;
};
struct ActivateControl {
  std::string instance_id;
  std::optional<std::chrono::milliseconds> duration;
  bool operator==(const ActivateControl &) const = default;
};
struct DismissControl {
  std::string context_id;
  bool operator==(const DismissControl &) const = default;
};

using ControlCommand =
    std::variant<OpenControl, CloseControl, ToggleControl, StatusControl, ModulesControl,
                 ReloadControl, RestartModuleControl, ActivateControl, DismissControl>;

enum class ControlErrorCode {
  invalid_request,
  unsupported_version,
  unknown_command,
  unknown_instance,
  unavailable_instance,
  unavailable_context,
  unknown_context,
  invalid_duration,
  restart_rejected,
  reload_rejected,
  internal_error
};

struct ControlError {
  ControlErrorCode code;
  std::string message;
};

enum class ControlModuleState { disabled, stopped, starting, running, backoff, stopping, failed };

struct ActiveContextStatus {
  std::string instance_id;
  std::string context_id;
  int priority;
};

struct ModuleControlStatus {
  std::string id;
  ControlModuleState state;
  bool available;

  bool operator==(const ModuleControlStatus &) const = default;
};

struct ControlStatus {
  IslandMode mode;
  std::optional<ActiveContextStatus> compact;
  std::optional<ActiveContextStatus> expanded;
  std::vector<ModuleControlStatus> modules;
  std::string socket;
};

struct ModulesStatus {
  std::vector<ModuleControlStatus> modules;
};

struct EmptyControlResult {};
using ControlResponseValue =
    std::variant<EmptyControlResult, ControlStatus, ModulesStatus, ControlError>;

class ControlResponse {
public:
  ControlResponse(EmptyControlResult result = {});
  ControlResponse(ControlStatus result);
  ControlResponse(ModulesStatus result);
  ControlResponse(ControlError error);

  [[nodiscard]] const ControlResponseValue &value() const noexcept;

private:
  ControlResponseValue value_;
};

struct ControlInvocation {
  ControlCommand command;
  bool json_output{false};
};

[[nodiscard]] std::expected<ControlCommand, ControlError>
parse_control_request(std::string_view line);
[[nodiscard]] std::string serialize_control_request(const ControlCommand &command);
[[nodiscard]] std::string serialize_control_response(const ControlResponse &response);
[[nodiscard]] std::expected<ControlResponse, std::string>
parse_control_response(std::string_view record);
[[nodiscard]] std::string serialize_control_result(const ControlResponse &response);
[[nodiscard]] std::expected<ControlInvocation, std::string>
parse_control_arguments(const std::vector<std::string> &arguments);
[[nodiscard]] std::string_view control_error_code_name(ControlErrorCode code);
[[nodiscard]] std::string_view control_module_state_name(ControlModuleState state);

} // namespace gisland
