#include "gisland/control.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace gisland {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;
using namespace std::chrono_literals;

constexpr std::chrono::milliseconds maximum_duration = 24h;

[[nodiscard]] std::unexpected<ControlError> fail(ControlErrorCode code, std::string message) {
  return std::unexpected(ControlError{code, std::move(message)});
}

[[nodiscard]] bool has_exact_fields(const Json &value,
                                    const std::set<std::string, std::less<>> &fields) {
  if (value.size() != fields.size()) {
    return false;
  }
  for (const auto &[key, field] : value.items()) {
    static_cast<void>(field);
    if (!fields.contains(key)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::expected<Json, ControlError> parse_json(std::string_view line) {
  bool duplicate = false;
  std::vector<std::set<std::string>> object_keys;
  const auto callback = [&duplicate, &object_keys](int /*depth*/, Json::parse_event_t event,
                                                   Json &parsed) {
    if (event == Json::parse_event_t::object_start) {
      object_keys.emplace_back();
    } else if (event == Json::parse_event_t::key) {
      duplicate = object_keys.empty() ||
                  !object_keys.back().insert(parsed.get<std::string>()).second || duplicate;
    } else if (event == Json::parse_event_t::object_end && !object_keys.empty()) {
      object_keys.pop_back();
    }
    return true;
  };

  Json parsed = Json::parse(line, callback, false);
  if (parsed.is_discarded() || duplicate) {
    return fail(ControlErrorCode::invalid_request, "request is not valid JSON");
  }
  return parsed;
}

[[nodiscard]] std::expected<std::string, ControlError> required_nonempty_string(const Json &request,
                                                                                const char *key) {
  const auto iterator = request.find(key);
  if (iterator == request.end() || !iterator->is_string()) {
    return fail(ControlErrorCode::invalid_request, std::string{key} + " must be a string");
  }
  auto value = iterator->get<std::string>();
  if (value.empty()) {
    return fail(ControlErrorCode::invalid_request, std::string{key} + " must not be empty");
  }
  return value;
}

[[nodiscard]] std::expected<std::chrono::milliseconds, ControlError>
request_duration(const Json &request) {
  const auto iterator = request.find("duration_ms");
  if (iterator == request.end() ||
      (!iterator->is_number_unsigned() && !iterator->is_number_integer())) {
    return fail(ControlErrorCode::invalid_duration, "duration_ms must be an integer");
  }
  std::int64_t value = 0;
  try {
    value = iterator->get<std::int64_t>();
  } catch (const Json::exception &) {
    return fail(ControlErrorCode::invalid_duration, "duration_ms is out of range");
  }
  if (value < 1 || value > maximum_duration.count()) {
    return fail(ControlErrorCode::invalid_duration, "duration_ms must be between 1 and 86400000");
  }
  return std::chrono::milliseconds{value};
}

[[nodiscard]] OrderedJson module_json(const ModuleControlStatus &module) {
  return OrderedJson{{"id", module.id},
                     {"state", control_module_state_name(module.state)},
                     {"available", module.available}};
}

[[nodiscard]] OrderedJson modules_json(const std::vector<ModuleControlStatus> &modules) {
  OrderedJson result = OrderedJson::array();
  for (const auto &module : modules) {
    result.push_back(module_json(module));
  }
  return result;
}

[[nodiscard]] OrderedJson status_json(const ControlStatus &status) {
  const auto context_json = [](const std::optional<ActiveContextStatus> &context) {
    if (!context) {
      return OrderedJson(nullptr);
    }
    return OrderedJson{{"instance_id", context->instance_id},
                       {"context_id", context->context_id},
                       {"priority", context->priority}};
  };
  return OrderedJson{{"format_version", 2},
                     {"mode", status.mode == IslandMode::expanded ? "expanded" : "compact"},
                     {"compact", context_json(status.compact)},
                     {"expanded", context_json(status.expanded)},
                     {"modules", modules_json(status.modules)},
                     {"socket", status.socket}};
}

[[nodiscard]] std::expected<std::optional<ActiveContextStatus>, std::string>
parse_active_context(const Json &value) {
  if (value.is_null()) {
    return std::nullopt;
  }
  if (!value.is_object() || !has_exact_fields(value, {"context_id", "instance_id", "priority"}) ||
      !value.at("context_id").is_string() || !value.at("instance_id").is_string() ||
      !value.at("priority").is_number_integer()) {
    return std::unexpected("response has an invalid active context: " + value.dump());
  }
  try {
    return ActiveContextStatus{value.at("instance_id").get<std::string>(),
                               value.at("context_id").get<std::string>(),
                               value.at("priority").get<int>()};
  } catch (const Json::exception &) {
    return std::unexpected("response active context priority is out of range");
  }
}

[[nodiscard]] std::optional<ControlErrorCode> parse_error_code(std::string_view name) {
  for (const auto code : {ControlErrorCode::invalid_request, ControlErrorCode::unsupported_version,
                          ControlErrorCode::unknown_command, ControlErrorCode::unknown_instance,
                          ControlErrorCode::unavailable_instance,
                          ControlErrorCode::unavailable_context, ControlErrorCode::unknown_context,
                          ControlErrorCode::invalid_duration, ControlErrorCode::restart_rejected,
                          ControlErrorCode::reload_rejected, ControlErrorCode::internal_error}) {
    if (control_error_code_name(code) == name) {
      return code;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<ControlModuleState> parse_module_state(std::string_view name) {
  for (const auto state :
       {ControlModuleState::disabled, ControlModuleState::stopped, ControlModuleState::starting,
        ControlModuleState::running, ControlModuleState::backoff, ControlModuleState::stopping,
        ControlModuleState::failed}) {
    if (control_module_state_name(state) == name) {
      return state;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::expected<std::vector<ModuleControlStatus>, std::string>
parse_modules(const Json &value) {
  if (!value.is_array()) {
    return std::unexpected("response modules must be an array");
  }
  std::vector<ModuleControlStatus> modules;
  modules.reserve(value.size());
  for (const auto &item : value) {
    if (!item.is_object() || !has_exact_fields(item, {"available", "id", "state"}) ||
        !item.at("id").is_string() || item.at("id").get_ref<const std::string &>().empty() ||
        !item.at("state").is_string() || !item.at("available").is_boolean()) {
      return std::unexpected("response contains an invalid module entry");
    }
    const auto state = parse_module_state(item.at("state").get_ref<const std::string &>());
    if (!state) {
      return std::unexpected("response contains an unknown module state");
    }
    modules.push_back({item.at("id").get<std::string>(), *state, item.at("available").get<bool>()});
  }
  return modules;
}

[[nodiscard]] std::expected<std::chrono::milliseconds, std::string>
parse_duration(std::string_view value) {
  if (value.empty()) {
    return std::unexpected("duration must not be empty");
  }
  std::chrono::milliseconds multiplier{};
  std::string_view digits;
  if (value.ends_with("ms")) {
    multiplier = 1ms;
    digits = value.substr(0, value.size() - 2);
  } else if (value.ends_with('s')) {
    multiplier = 1s;
    digits = value.substr(0, value.size() - 1);
  } else if (value.ends_with('m')) {
    multiplier = 1min;
    digits = value.substr(0, value.size() - 1);
  } else if (value.ends_with('h')) {
    multiplier = 1h;
    digits = value.substr(0, value.size() - 1);
  } else {
    return std::unexpected("duration requires ms, s, m, or h");
  }

  std::uint64_t count = 0;
  const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), count);
  if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size()) {
    return std::unexpected("duration requires an unsigned integer");
  }
  const auto multiplier_count = static_cast<std::uint64_t>(multiplier.count());
  if (count == 0 ||
      count > static_cast<std::uint64_t>(maximum_duration.count()) / multiplier_count) {
    return std::unexpected("duration must be between 1ms and 24h");
  }
  return std::chrono::milliseconds{static_cast<std::int64_t>(count * multiplier_count)};
}

} // namespace

ControlResponse::ControlResponse(EmptyControlResult result) : value_(result) {}
ControlResponse::ControlResponse(ControlStatus result) : value_(std::move(result)) {}
ControlResponse::ControlResponse(ModulesStatus result) : value_(std::move(result)) {}
ControlResponse::ControlResponse(ControlError error) : value_(std::move(error)) {}

const ControlResponseValue &ControlResponse::value() const noexcept { return value_; }

std::expected<ControlCommand, ControlError> parse_control_request(std::string_view line) {
  auto parsed = parse_json(line);
  if (!parsed) {
    return std::unexpected(parsed.error());
  }
  if (!parsed->is_object()) {
    return fail(ControlErrorCode::invalid_request, "request must be an object");
  }

  const auto version = parsed->find("version");
  if (version == parsed->end() || !version->is_number_integer()) {
    return fail(ControlErrorCode::invalid_request, "version must be an integer");
  }
  if (*version != 1) {
    return fail(ControlErrorCode::unsupported_version, "control protocol version is unsupported");
  }
  const auto command_field = parsed->find("command");
  if (command_field == parsed->end() || !command_field->is_string()) {
    return fail(ControlErrorCode::invalid_request, "command must be a string");
  }
  const auto command = command_field->get<std::string>();

  const std::set<std::string, std::less<>> basic_fields{"command", "version"};
  if (command == "open" || command == "close" || command == "toggle" || command == "status" ||
      command == "modules" || command == "reload") {
    if (!has_exact_fields(*parsed, basic_fields)) {
      return fail(ControlErrorCode::invalid_request, "request has an unexpected property");
    }
    if (command == "open") {
      return ControlCommand{OpenControl{}};
    }
    if (command == "close") {
      return ControlCommand{CloseControl{}};
    }
    if (command == "toggle") {
      return ControlCommand{ToggleControl{}};
    }
    if (command == "status") {
      return ControlCommand{StatusControl{}};
    }
    if (command == "reload") {
      return ControlCommand{ReloadControl{}};
    }
    return ControlCommand{ModulesControl{}};
  }

  if (command != "module-restart" && command != "activate" && command != "dismiss") {
    return fail(ControlErrorCode::unknown_command, "control command is unknown");
  }
  if (command == "module-restart") {
    if (!has_exact_fields(*parsed, {"command", "instance", "version"})) {
      return fail(ControlErrorCode::invalid_request, "request has an unexpected property");
    }
    auto instance = required_nonempty_string(*parsed, "instance");
    if (!instance) {
      return std::unexpected(instance.error());
    }
    return ControlCommand{RestartModuleControl{std::move(*instance)}};
  }
  if (command == "dismiss") {
    if (!has_exact_fields(*parsed, {"command", "context_id", "version"})) {
      return fail(ControlErrorCode::invalid_request, "request has an unexpected property");
    }
    auto context_id = required_nonempty_string(*parsed, "context_id");
    if (!context_id) {
      return std::unexpected(context_id.error());
    }
    return ControlCommand{DismissControl{std::move(*context_id)}};
  }

  const bool has_duration = parsed->contains("duration_ms");
  if (!has_exact_fields(
          *parsed,
          has_duration
              ? std::set<std::string, std::less<>>{"command", "duration_ms", "instance", "version"}
              : std::set<std::string, std::less<>>{"command", "instance", "version"})) {
    return fail(ControlErrorCode::invalid_request, "request has an unexpected property");
  }
  auto instance = required_nonempty_string(*parsed, "instance");
  if (!instance) {
    return std::unexpected(instance.error());
  }
  std::optional<std::chrono::milliseconds> duration;
  if (has_duration) {
    auto parsed_duration = request_duration(*parsed);
    if (!parsed_duration) {
      return std::unexpected(parsed_duration.error());
    }
    duration = *parsed_duration;
  }
  return ControlCommand{ActivateControl{std::move(*instance), duration}};
}

std::string serialize_control_request(const ControlCommand &command) {
  OrderedJson result;
  std::visit(
      [&result](const auto &typed) {
        using Type = std::decay_t<decltype(typed)>;
        result["version"] = 1;
        if constexpr (std::is_same_v<Type, OpenControl>) {
          result["command"] = "open";
        } else if constexpr (std::is_same_v<Type, CloseControl>) {
          result["command"] = "close";
        } else if constexpr (std::is_same_v<Type, ToggleControl>) {
          result["command"] = "toggle";
        } else if constexpr (std::is_same_v<Type, StatusControl>) {
          result["command"] = "status";
        } else if constexpr (std::is_same_v<Type, ModulesControl>) {
          result["command"] = "modules";
        } else if constexpr (std::is_same_v<Type, ReloadControl>) {
          result["command"] = "reload";
        } else if constexpr (std::is_same_v<Type, RestartModuleControl>) {
          result["command"] = "module-restart";
          result["instance"] = typed.instance_id;
        } else if constexpr (std::is_same_v<Type, ActivateControl>) {
          result["command"] = "activate";
          result["instance"] = typed.instance_id;
          if (typed.duration) {
            result["duration_ms"] = typed.duration->count();
          }
        } else if constexpr (std::is_same_v<Type, DismissControl>) {
          result["command"] = "dismiss";
          result["context_id"] = typed.context_id;
        }
      },
      command);
  return result.dump() + '\n';
}

std::string serialize_control_response(const ControlResponse &response) {
  OrderedJson result;
  result["version"] = 1;
  std::visit(
      [&result](const auto &typed) {
        using Type = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Type, ControlError>) {
          result["ok"] = false;
          result["error"] = OrderedJson{{"code", control_error_code_name(typed.code)},
                                        {"message", typed.message}};
        } else {
          result["ok"] = true;
          if constexpr (std::is_same_v<Type, EmptyControlResult>) {
            result["result"] = OrderedJson::object();
          } else if constexpr (std::is_same_v<Type, ControlStatus>) {
            result["result"] = status_json(typed);
          } else if constexpr (std::is_same_v<Type, ModulesStatus>) {
            result["result"] = OrderedJson{{"modules", modules_json(typed.modules)}};
          }
        }
      },
      response.value());
  return result.dump() + '\n';
}

std::expected<ControlResponse, std::string> parse_control_response(std::string_view record) {
  auto parsed = parse_json(record);
  if (!parsed || !parsed->is_object()) {
    return std::unexpected("response is not valid JSON object");
  }
  const auto version = parsed->find("version");
  const auto ok = parsed->find("ok");
  if (version == parsed->end() || !version->is_number_integer() || *version != 1 ||
      ok == parsed->end() || !ok->is_boolean()) {
    return std::unexpected("response has an invalid envelope");
  }
  if (!ok->get<bool>()) {
    if (!has_exact_fields(*parsed, {"error", "ok", "version"}) ||
        !parsed->at("error").is_object() ||
        !has_exact_fields(parsed->at("error"), {"code", "message"}) ||
        !parsed->at("error").at("code").is_string() ||
        !parsed->at("error").at("message").is_string()) {
      return std::unexpected("response has an invalid error envelope");
    }
    const auto code =
        parse_error_code(parsed->at("error").at("code").get_ref<const std::string &>());
    if (!code) {
      return std::unexpected("response has an unknown error code");
    }
    return ControlResponse{
        ControlError{*code, parsed->at("error").at("message").get<std::string>()}};
  }
  if (!has_exact_fields(*parsed, {"ok", "result", "version"}) ||
      !parsed->at("result").is_object()) {
    return std::unexpected("response has an invalid success envelope");
  }
  const Json &result = parsed->at("result");
  if (result.empty()) {
    return ControlResponse{};
  }
  if (has_exact_fields(result, {"modules"})) {
    auto modules = parse_modules(result.at("modules"));
    if (!modules) {
      return std::unexpected(modules.error());
    }
    return ControlResponse{ModulesStatus{std::move(*modules)}};
  }
  if (!result.contains("format_version") || !result.at("format_version").is_number_integer() ||
      !result.contains("mode") || !result.at("mode").is_string() || !result.contains("modules") ||
      !result.contains("socket") || !result.at("socket").is_string()) {
    return std::unexpected("response has an invalid status result");
  }
  const int format_version = result.at("format_version").get<int>();
  const bool legacy = format_version == 1;
  if ((legacy && !has_exact_fields(
                     result, {"active_context", "format_version", "mode", "modules", "socket"})) ||
      (!legacy &&
       (format_version != 2 || !has_exact_fields(result, {"compact", "expanded", "format_version",
                                                          "mode", "modules", "socket"})))) {
    return std::unexpected("response has an invalid status result");
  }
  const auto mode_name = result.at("mode").get<std::string>();
  if (mode_name != "compact" && mode_name != "expanded") {
    return std::unexpected("response has an unknown mode");
  }
  std::optional<ActiveContextStatus> compact;
  std::optional<ActiveContextStatus> expanded;
  if (legacy) {
    auto active = parse_active_context(result.at("active_context"));
    if (!active) {
      return std::unexpected(active.error());
    }
    if (mode_name == "expanded") {
      expanded = std::move(*active);
    } else {
      compact = std::move(*active);
    }
  } else {
    auto parsed_compact = parse_active_context(result.at("compact"));
    auto parsed_expanded = parse_active_context(result.at("expanded"));
    if (!parsed_compact) {
      return std::unexpected(parsed_compact.error());
    }
    if (!parsed_expanded) {
      return std::unexpected(parsed_expanded.error());
    }
    compact = std::move(*parsed_compact);
    expanded = std::move(*parsed_expanded);
  }
  auto modules = parse_modules(result.at("modules"));
  if (!modules) {
    return std::unexpected(modules.error());
  }
  return ControlResponse{ControlStatus{
      mode_name == "expanded" ? IslandMode::expanded : IslandMode::compact, std::move(compact),
      std::move(expanded), std::move(*modules), result.at("socket").get<std::string>()}};
}

std::string serialize_control_result(const ControlResponse &response) {
  return std::visit(
      [](const auto &typed) -> std::string {
        using Type = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Type, ControlStatus>) {
          return status_json(typed).dump() + '\n';
        } else if constexpr (std::is_same_v<Type, ModulesStatus>) {
          return OrderedJson{{"modules", modules_json(typed.modules)}}.dump() + '\n';
        }
        return OrderedJson::object().dump() + '\n';
      },
      response.value());
}

std::expected<ControlInvocation, std::string>
parse_control_arguments(const std::vector<std::string> &arguments) {
  if (arguments.size() == 1) {
    if (arguments[0] == "open") {
      return ControlInvocation{OpenControl{}};
    }
    if (arguments[0] == "close") {
      return ControlInvocation{CloseControl{}};
    }
    if (arguments[0] == "toggle") {
      return ControlInvocation{ToggleControl{}};
    }
    if (arguments[0] == "status") {
      return ControlInvocation{StatusControl{}};
    }
    if (arguments[0] == "modules") {
      return ControlInvocation{ModulesControl{}};
    }
    if (arguments[0] == "reload") {
      return ControlInvocation{ReloadControl{}};
    }
  }
  if (arguments.size() == 2 && arguments[0] == "status" && arguments[1] == "--json") {
    return ControlInvocation{StatusControl{}, true};
  }
  if (arguments.size() == 3 && arguments[0] == "module" && arguments[1] == "restart" &&
      !arguments[2].empty()) {
    return ControlInvocation{RestartModuleControl{arguments[2]}};
  }
  if (arguments.size() == 2 && arguments[0] == "activate" && !arguments[1].empty()) {
    return ControlInvocation{ActivateControl{arguments[1], std::nullopt}};
  }
  if (arguments.size() == 4 && arguments[0] == "activate" && !arguments[1].empty() &&
      arguments[2] == "--duration") {
    auto duration = parse_duration(arguments[3]);
    if (!duration) {
      return std::unexpected(duration.error());
    }
    return ControlInvocation{ActivateControl{arguments[1], *duration}};
  }
  if (arguments.size() == 2 && arguments[0] == "dismiss" && !arguments[1].empty()) {
    return ControlInvocation{DismissControl{arguments[1]}};
  }
  return std::unexpected("invalid gislandctl arguments");
}

std::string_view control_error_code_name(ControlErrorCode code) {
  switch (code) {
  case ControlErrorCode::invalid_request:
    return "invalid_request";
  case ControlErrorCode::unsupported_version:
    return "unsupported_version";
  case ControlErrorCode::unknown_command:
    return "unknown_command";
  case ControlErrorCode::unknown_instance:
    return "unknown_instance";
  case ControlErrorCode::unavailable_instance:
    return "unavailable_instance";
  case ControlErrorCode::unavailable_context:
    return "unavailable_context";
  case ControlErrorCode::unknown_context:
    return "unknown_context";
  case ControlErrorCode::invalid_duration:
    return "invalid_duration";
  case ControlErrorCode::restart_rejected:
    return "restart_rejected";
  case ControlErrorCode::reload_rejected:
    return "reload_rejected";
  case ControlErrorCode::internal_error:
    return "internal_error";
  }
  return "internal_error";
}

std::string_view control_module_state_name(ControlModuleState state) {
  switch (state) {
  case ControlModuleState::disabled:
    return "disabled";
  case ControlModuleState::stopped:
    return "stopped";
  case ControlModuleState::starting:
    return "starting";
  case ControlModuleState::running:
    return "running";
  case ControlModuleState::backoff:
    return "backoff";
  case ControlModuleState::stopping:
    return "stopping";
  case ControlModuleState::failed:
    return "failed";
  }
  return "failed";
}

} // namespace gisland
