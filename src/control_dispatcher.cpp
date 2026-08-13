#include "gisland/control_dispatcher.hpp"

#include <limits>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <variant>

namespace gisland {
namespace {

using namespace std::chrono_literals;

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
                                     RestartRequest request_restart, std::string socket_path,
                                     ReloadRequest request_reload, ActionRequest request_action,
                                     std::uint64_t first_invocation_id)
    : runtime_(runtime), mode_(mode), request_restart_(std::move(request_restart)),
      request_reload_(std::move(request_reload)), request_action_(std::move(request_action)),
      socket_path_(std::move(socket_path)), next_invocation_id_(first_invocation_id) {}

ControlResponse ControlDispatcher::dispatch(const ControlCommand &command, MonotonicTime now) {
  return std::visit(
      [this, now](const auto &typed_command) -> ControlResponse {
        using Command = std::decay_t<decltype(typed_command)>;
        if constexpr (std::is_same_v<Command, OpenControl>) {
          const auto selected = runtime_.active(ViewSlot::expanded, now);
          if (!mode_.open(selected.context != nullptr)) {
            return control_error(ControlErrorCode::unavailable_context,
                                 "the active context has no expanded view");
          }
        } else if constexpr (std::is_same_v<Command, CloseControl>) {
          mode_.close();
        } else if constexpr (std::is_same_v<Command, ToggleControl>) {
          const auto selected = runtime_.active(ViewSlot::expanded, now);
          if (!mode_.toggle(selected.context != nullptr)) {
            return control_error(ControlErrorCode::unavailable_context,
                                 "the active context has no expanded view");
          }
        } else if constexpr (std::is_same_v<Command, StatusControl>) {
          return status(now);
        } else if constexpr (std::is_same_v<Command, ModulesControl>) {
          return ControlResponse{ModulesStatus{modules(now)}};
        } else if constexpr (std::is_same_v<Command, ReloadControl>) {
          if (!request_reload_) {
            return control_error(ControlErrorCode::reload_rejected, "reload is unavailable");
          }
          const auto reloaded = request_reload_(now);
          if (!reloaded) {
            return control_error(ControlErrorCode::reload_rejected, reloaded.error());
          }
          cancel_all();
        } else if constexpr (std::is_same_v<Command, RestartModuleControl>) {
          return restart(typed_command.instance_id, now);
        } else if constexpr (std::is_same_v<Command, ActivateControl>) {
          const auto activated =
              runtime_.activate(typed_command.instance_id, typed_command.duration, now);
          if (!activated) {
            return control_error(control_error_code(activated.error().code),
                                 activated.error().message);
          }
          mode_.close();
        } else if constexpr (std::is_same_v<Command, ActivateOpenControl>) {
          const auto activated = runtime_.activate(typed_command.instance_id, std::nullopt, now);
          if (!activated) {
            return control_error(control_error_code(activated.error().code),
                                 activated.error().message);
          }
          const auto selected = runtime_.active(ViewSlot::expanded, now);
          if (!mode_.open(selected.context != nullptr &&
                          selected.context->key.instance_id == typed_command.instance_id)) {
            return control_error(ControlErrorCode::unavailable_context,
                                 "the activated context has no expanded view");
          }
        } else if constexpr (std::is_same_v<Command, DismissControl>) {
          const auto dismissed = runtime_.dismiss_active(
              typed_command.context_id,
              mode_.mode() == IslandMode::expanded ? ViewSlot::expanded : ViewSlot::compact, now);
          if (!dismissed) {
            return control_error(control_error_code(dismissed.error().code),
                                 dismissed.error().message);
          }
        } else if constexpr (std::is_same_v<Command, ActionControl>) {
          return control_error(ControlErrorCode::action_delivery_failed,
                               "action commands require deferred dispatch");
        }
        return ControlResponse{};
      },
      command);
}

ControlDispatchResult ControlDispatcher::dispatch_deferred(const ControlCommand &command,
                                                           MonotonicTime now) {
  if (const auto *action_command = std::get_if<ActionControl>(&command)) {
    return action(*action_command, now);
  }
  return dispatch(command, now);
}

void ControlDispatcher::consume(const RestartCompletedEvent &event) {
  const auto pending = pending_restarts_.find(event.instance_id);
  if (pending != pending_restarts_.end() && pending->second == event.generation) {
    pending_restarts_.erase(pending);
  }
}

void ControlDispatcher::consume(const ActionDeliveryEvent &event) {
  const auto pending = pending_actions_.find(event.invocation_id);
  if (pending == pending_actions_.end() || pending->second.instance_id != event.instance_id ||
      pending->second.generation != event.generation || pending->second.deadline.has_value()) {
    return;
  }
  if (!event.succeeded) {
    complete(event.invocation_id, control_error(ControlErrorCode::action_delivery_failed,
                                                "the action could not be delivered to the module"));
    return;
  }
  pending->second.deadline = event.at + 2s;
}

ActionEventResult ControlDispatcher::consume(const ModuleMessageEvent &event) {
  const auto *result = std::get_if<ActionResultMessage>(&event.message);
  if (result == nullptr) {
    return ActionEventResult::stale;
  }
  if (!result->invocation_id) {
    const bool requires_correlation =
        std::ranges::any_of(pending_actions_, [&event, result](const auto &entry) {
          const auto &pending = entry.second;
          return pending.instance_id == event.instance_id &&
                 pending.generation == event.generation && pending.action_id == result->action_id;
        });
    return requires_correlation ? ActionEventResult::protocol_error : ActionEventResult::stale;
  }
  const auto pending = pending_actions_.find(*result->invocation_id);
  if (pending == pending_actions_.end() || pending->second.instance_id != event.instance_id ||
      pending->second.generation != event.generation ||
      pending->second.action_id != result->action_id || !pending->second.deadline) {
    return ActionEventResult::stale;
  }
  if (result->accepted) {
    complete(*result->invocation_id, ControlResponse{});
  } else {
    complete(*result->invocation_id,
             control_error(ControlErrorCode::action_rejected,
                           result->message.value_or("the module rejected the action")));
  }
  return ActionEventResult::consumed;
}

void ControlDispatcher::consume(const ProcessExitedEvent &event) {
  cancel_generation(event.instance_id, event.generation);
}

void ControlDispatcher::consume(const ContextsRemovedEvent &event) {
  cancel_generation(event.instance_id, event.generation);
}

void ControlDispatcher::consume(const ProcessStartedEvent &event) {
  std::vector<std::uint64_t> replaced;
  for (const auto &[invocation_id, pending] : pending_actions_) {
    if (pending.instance_id == event.instance_id && pending.generation != event.generation) {
      replaced.push_back(invocation_id);
    }
  }
  for (const auto invocation_id : replaced) {
    complete(invocation_id,
             control_error(ControlErrorCode::action_cancelled, "the module process was replaced"));
  }
}

void ControlDispatcher::expire(MonotonicTime now) {
  std::vector<std::uint64_t> expired;
  for (const auto &[invocation_id, pending] : pending_actions_) {
    if (pending.deadline && now >= *pending.deadline) {
      expired.push_back(invocation_id);
    }
  }
  for (const auto invocation_id : expired) {
    complete(invocation_id,
             control_error(ControlErrorCode::action_timeout, "the module action timed out"));
  }
}

bool ControlDispatcher::cancel(PendingControlToken token) {
  return pending_actions_.erase(token.value) != 0;
}

void ControlDispatcher::cancel_generation(std::string_view instance_id, std::uint64_t generation) {
  std::vector<std::uint64_t> cancelled;
  for (const auto &[invocation_id, pending] : pending_actions_) {
    if (pending.instance_id == instance_id && pending.generation == generation) {
      cancelled.push_back(invocation_id);
    }
  }
  for (const auto invocation_id : cancelled) {
    complete(invocation_id,
             control_error(ControlErrorCode::action_cancelled, "the module process stopped"));
  }
}

void ControlDispatcher::cancel_instance(std::string_view instance_id) {
  std::vector<std::uint64_t> cancelled;
  for (const auto &[invocation_id, pending] : pending_actions_) {
    if (pending.instance_id == instance_id) {
      cancelled.push_back(invocation_id);
    }
  }
  for (const auto invocation_id : cancelled) {
    complete(invocation_id,
             control_error(ControlErrorCode::action_cancelled, "the module instance was removed"));
  }
}

void ControlDispatcher::cancel_all() {
  std::vector<std::uint64_t> cancelled;
  cancelled.reserve(pending_actions_.size());
  for (const auto &[invocation_id, pending] : pending_actions_) {
    static_cast<void>(pending);
    cancelled.push_back(invocation_id);
  }
  for (const auto invocation_id : cancelled) {
    complete(invocation_id,
             control_error(ControlErrorCode::action_cancelled, "control actions were cancelled"));
  }
}

std::vector<CompletedControlAction> ControlDispatcher::take_completed() {
  return std::exchange(completed_actions_, {});
}

std::size_t ControlDispatcher::pending_action_count() const noexcept {
  return pending_actions_.size();
}

ControlResponse ControlDispatcher::status(MonotonicTime now) {
  const auto selected = runtime_.selections(now);
  const auto status = [](const RuntimeSelection &selection) -> std::optional<ActiveContextStatus> {
    if (selection.context == nullptr) {
      return std::nullopt;
    }
    return ActiveContextStatus{selection.context->key.instance_id,
                               selection.context->key.context_id, selection.context->priority};
  };
  return ControlResponse{ControlStatus{mode_.mode(), status(selected.compact),
                                       status(selected.expanded), modules(now), socket_path_}};
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
  cancel_instance(instance_id);
  pending_restarts_.emplace(instance_id, generation);
  return ControlResponse{};
}

ControlDispatchResult ControlDispatcher::action(const ActionControl &command, MonotonicTime now) {
  static_cast<void>(now);
  const auto target = runtime_.action_target(command.instance_id);
  if (!target) {
    return control_error(control_error_code(target.error().code), target.error().message);
  }
  if (target->protocol < ProtocolVersion{1, 8}) {
    return control_error(ControlErrorCode::unsupported_module_protocol,
                         "module instance did not negotiate protocol 1.8");
  }
  if (!request_action_) {
    return control_error(ControlErrorCode::action_delivery_failed,
                         "module action delivery is unavailable");
  }
  const auto invocation_id = allocate_invocation_id();
  if (!invocation_id) {
    return control_error(ControlErrorCode::internal_error,
                         "no module action invocation identifier is available");
  }
  const PendingControlToken token{*invocation_id};
  pending_actions_.emplace(*invocation_id,
                           PendingAction{token, command.instance_id, target->generation,
                                         command.action_id, std::nullopt});
  const auto submitted = request_action_(
      command.instance_id, target->generation,
      ActionMessage{command.action_id, command.value, std::optional{*invocation_id}});
  if (!submitted) {
    pending_actions_.erase(*invocation_id);
    return control_error(ControlErrorCode::action_delivery_failed,
                         "the action delivery request could not be queued");
  }
  return token;
}

std::optional<std::uint64_t> ControlDispatcher::allocate_invocation_id() {
  const std::uint64_t first = next_invocation_id_ == 0 ? 1 : next_invocation_id_;
  std::uint64_t candidate = first;
  do {
    if (!pending_actions_.contains(candidate)) {
      next_invocation_id_ =
          candidate == std::numeric_limits<std::uint64_t>::max() ? 1 : candidate + 1;
      return candidate;
    }
    candidate = candidate == std::numeric_limits<std::uint64_t>::max() ? 1 : candidate + 1;
  } while (candidate != first);
  return std::nullopt;
}

void ControlDispatcher::complete(std::uint64_t invocation_id, ControlResponse response) {
  const auto pending = pending_actions_.find(invocation_id);
  if (pending == pending_actions_.end()) {
    return;
  }
  completed_actions_.push_back(CompletedControlAction{pending->second.token, std::move(response)});
  pending_actions_.erase(pending);
}

} // namespace gisland
