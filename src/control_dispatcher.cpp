#include "gisland/control_dispatcher.hpp"

#include <limits>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <variant>

namespace gisland {
namespace {

[[nodiscard]] ControlResponse control_error(ControlErrorCode code, std::string message) {
  return ControlResponse{ControlError{code, std::move(message)}};
}

[[nodiscard]] ControlErrorCode control_error_code(RuntimeErrorCode code) {
  switch (code) {
  case RuntimeErrorCode::unknown_instance:
    return ControlErrorCode::unknown_instance;
  case RuntimeErrorCode::disabled_instance:
  case RuntimeErrorCode::unavailable_instance:
    return ControlErrorCode::unavailable_instance;
  case RuntimeErrorCode::unknown_context:
    return ControlErrorCode::unknown_context;
  case RuntimeErrorCode::missing_view:
  case RuntimeErrorCode::invalid_snapshot:
    return ControlErrorCode::internal_error;
  }
  return ControlErrorCode::internal_error;
}

[[nodiscard]] ControlModuleState control_state(const RuntimeModuleStatus &module) {
  if (!module.enabled) {
    return ControlModuleState::disabled;
  }
  switch (module.state) {
  case ModuleState::stopped:
    return ControlModuleState::stopped;
  case ModuleState::starting:
    return ControlModuleState::starting;
  case ModuleState::running:
    return ControlModuleState::running;
  case ModuleState::backoff:
    return ControlModuleState::backoff;
  case ModuleState::stopping:
    return ControlModuleState::stopping;
  case ModuleState::failed:
    return ControlModuleState::failed;
  }
  return ControlModuleState::failed;
}

} // namespace

ControlDispatcher::ControlDispatcher(RuntimeCoordinator &runtime, OverlayModeController &mode,
                                     RestartRequest request_restart, std::string socket_path)
    : runtime_(runtime), mode_(mode), request_restart_(std::move(request_restart)),
      socket_path_(std::move(socket_path)) {}

ControlResponse ControlDispatcher::dispatch(const ControlCommand &command, MonotonicTime now) {
  return std::visit(
      [this, now](const auto &typed_command) -> ControlResponse {
        using Command = std::decay_t<decltype(typed_command)>;
        if constexpr (std::is_same_v<Command, OpenControl>) {
          const auto selected = runtime_.active(now);
          if (selected.context == nullptr || !mode_.open(selected.context->expanded.has_value())) {
            return control_error(ControlErrorCode::unavailable_context,
                                 "the active context has no expanded view");
          }
        } else if constexpr (std::is_same_v<Command, CloseControl>) {
          mode_.close();
        } else if constexpr (std::is_same_v<Command, ToggleControl>) {
          const auto selected = runtime_.active(now);
          const bool has_expanded =
              selected.context != nullptr && selected.context->expanded.has_value();
          if (!mode_.toggle(has_expanded)) {
            return control_error(ControlErrorCode::unavailable_context,
                                 "the active context has no expanded view");
          }
        } else if constexpr (std::is_same_v<Command, StatusControl>) {
          return status(now);
        } else if constexpr (std::is_same_v<Command, ModulesControl>) {
          return ControlResponse{ModulesStatus{modules(now)}};
        } else if constexpr (std::is_same_v<Command, RestartModuleControl>) {
          return restart(typed_command.instance_id, now);
        } else if constexpr (std::is_same_v<Command, ActivateControl>) {
          const auto activated =
              runtime_.activate(typed_command.instance_id, typed_command.duration, now);
          if (!activated) {
            return control_error(control_error_code(activated.error().code),
                                 activated.error().message);
          }
        } else if constexpr (std::is_same_v<Command, DismissControl>) {
          const auto dismissed = runtime_.dismiss_active(typed_command.context_id, now);
          if (!dismissed) {
            return control_error(control_error_code(dismissed.error().code),
                                 dismissed.error().message);
          }
        }
        return ControlResponse{};
      },
      command);
}

void ControlDispatcher::consume(const RestartCompletedEvent &event) {
  const auto pending = pending_restarts_.find(event.instance_id);
  if (pending != pending_restarts_.end() && pending->second == event.generation) {
    pending_restarts_.erase(pending);
  }
}

ControlResponse ControlDispatcher::status(MonotonicTime now) {
  const auto selected = runtime_.active(now);
  std::optional<ActiveContextStatus> active_context;
  if (selected.context != nullptr) {
    active_context =
        ActiveContextStatus{selected.context->key.instance_id, selected.context->key.context_id,
                            selected.context->priority};
  }
  return ControlResponse{
      ControlStatus{mode_.mode(), std::move(active_context), modules(now), socket_path_}};
}

std::vector<ModuleControlStatus> ControlDispatcher::modules(MonotonicTime now) {
  const auto runtime_modules = runtime_.module_statuses(now);
  std::vector<ModuleControlStatus> result;
  result.reserve(runtime_modules.size());
  for (const auto &module : runtime_modules) {
    result.push_back(ModuleControlStatus{module.id, control_state(module), module.available});
  }
  return result;
}

ControlResponse ControlDispatcher::restart(std::string_view instance_id, MonotonicTime now) {
  const auto runtime_modules = runtime_.module_statuses(now);
  const auto module = std::ranges::find(runtime_modules, instance_id, &RuntimeModuleStatus::id);
  if (module == runtime_modules.end()) {
    return control_error(ControlErrorCode::unknown_instance,
                         "module instance '" + std::string{instance_id} + "' does not exist");
  }
  if (!module->enabled) {
    return control_error(ControlErrorCode::unavailable_instance,
                         "module instance '" + std::string{instance_id} + "' is disabled");
  }
  if (pending_restarts_.contains(std::string{instance_id})) {
    return control_error(ControlErrorCode::restart_rejected,
                         "a restart is already pending for module instance '" +
                             std::string{instance_id} + "'");
  }
  if (next_restart_generation_ == std::numeric_limits<std::uint64_t>::max()) {
    return control_error(ControlErrorCode::internal_error, "the restart could not be requested");
  }

  const std::uint64_t generation = next_restart_generation_++;
  const auto requested = request_restart_(std::string{instance_id}, generation);
  if (!requested) {
    return control_error(ControlErrorCode::restart_rejected,
                         "the module restart request could not be queued");
  }
  pending_restarts_.emplace(instance_id, generation);
  return ControlResponse{};
}

} // namespace gisland
