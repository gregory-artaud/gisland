#include "gisland/runtime.hpp"

#include <chrono>
#include <optional>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace gisland {
namespace {

[[nodiscard]] RuntimeError runtime_error(RuntimeErrorCode code, std::string instance_id,
                                         std::string message) {
  return RuntimeError{code, std::move(instance_id), std::move(message)};
}

[[nodiscard]] nlohmann::json config_json(const ConfigValue &value);

[[nodiscard]] nlohmann::json config_json(const ConfigValue::Table &table) {
  nlohmann::json result = nlohmann::json::object();
  for (const auto &[key, value] : table) {
    result[key] = config_json(value);
  }
  return result;
}

[[nodiscard]] nlohmann::json config_json(const ConfigValue &value) {
  return std::visit(
      [](const auto &typed_value) -> nlohmann::json {
        using Value = std::decay_t<decltype(typed_value)>;
        if constexpr (std::is_same_v<Value, ConfigValue::Array>) {
          nlohmann::json result = nlohmann::json::array();
          for (const auto &item : typed_value) {
            result.push_back(config_json(item));
          }
          return result;
        } else if constexpr (std::is_same_v<Value, ConfigValue::Table>) {
          return config_json(typed_value);
        } else {
          return typed_value;
        }
      },
      value.value);
}

} // namespace

ModuleStartRequest make_module_start_request(const ModuleInstanceConfig &config, std::string locale,
                                             std::string timezone) {
  std::vector<std::string> capabilities;
  if (config.view) {
    capabilities.emplace_back("data-snapshots");
  }
  return ModuleStartRequest{
      .instance_id = config.id,
      .process =
          ProcessSpec{
              .argv = config.command,
              .environment = config.environment,
              .working_directory = config.working_directory,
          },
      .init =
          InitMessage{
              .minimum = {1, 0},
              .maximum = {1, 1},
              .instance_id = config.id,
              .capabilities = std::move(capabilities),
              .configuration = config_json(config.options),
              .locale = std::move(locale),
              .timezone = std::move(timezone),
          },
      .restart = config.restart,
      .timings = config.timings,
  };
}

RuntimeCoordinator::RuntimeCoordinator(const AppConfig &config)
    : arbiter_(ContextKey{config.default_module, std::string{configured_context_id}}) {
  for (const auto &module : config.modules) {
    configured_instances_.emplace_back(module.id, module.enabled);
    if (!module.enabled) {
      continue;
    }
    module_states_.emplace(module.id, ModuleState::stopped);
    enabled_instances_.push_back(module.id);
    if (module.view) {
      views_.emplace(module.id, ModuleViewState{module.view->compact, module.view->expanded});
    }
  }
}

std::expected<void, RuntimeError> RuntimeCoordinator::consume(const SupervisorEvent &event) {
  return std::visit(
      [this](const auto &typed_event) -> std::expected<void, RuntimeError> {
        using Event = std::decay_t<decltype(typed_event)>;
        if constexpr (std::is_same_v<Event, ModuleMessageEvent>) {
          return consume_message(typed_event);
        } else if constexpr (std::is_same_v<Event, StateChangedEvent>) {
          module_states_.insert_or_assign(typed_event.instance_id, typed_event.transition.to);
        } else if constexpr (std::is_same_v<Event, ContextsRemovedEvent>) {
          arbiter_.dismiss_instance(typed_event.instance_id);
          ready_instances_.erase(typed_event.instance_id);
          visibility_.erase(typed_event.instance_id);
          ++revision_;
        }
        return {};
      },
      event);
}

std::expected<void, RuntimeError>
RuntimeCoordinator::consume_message(const ModuleMessageEvent &event) {
  return std::visit(
      [this, &event](const auto &message) -> std::expected<void, RuntimeError> {
        using Message = std::decay_t<decltype(message)>;
        if constexpr (std::is_same_v<Message, ReadyMessage>) {
          ready_instances_.insert(event.instance_id);
        } else if constexpr (std::is_same_v<Message, PublishMessage>) {
          arbiter_.publish(
              PublishedContext{
                  .key = {event.instance_id, message.context_id},
                  .priority = message.priority,
                  .expires_at = message.expires_in ? std::optional{event.at + *message.expires_in}
                                                   : std::nullopt,
                  .compact = message.compact,
                  .expanded = message.expanded,
              },
              event.at);
          ++revision_;
        } else if constexpr (std::is_same_v<Message, DismissMessage>) {
          arbiter_.dismiss(ContextKey{event.instance_id, message.context_id});
          ++revision_;
        } else if constexpr (std::is_same_v<Message, DataMessage>) {
          auto view = views_.find(event.instance_id);
          if (view == views_.end()) {
            return std::unexpected(runtime_error(RuntimeErrorCode::missing_view, event.instance_id,
                                                 "data snapshot has no configured view"));
          }
          auto applied = view->second.apply(message.value);
          if (!applied) {
            return std::unexpected(
                runtime_error(RuntimeErrorCode::invalid_snapshot, event.instance_id,
                              "snapshot failed at template '" + applied.error().template_path +
                                  "' and data '" + applied.error().data_path + "'"));
          }
          const auto &instantiated = *view->second.views();
          arbiter_.publish(
              PublishedContext{
                  .key = {event.instance_id, std::string{configured_context_id}},
                  .priority = 0,
                  .expires_at = std::nullopt,
                  .compact = instantiated.compact,
                  .expanded = instantiated.expanded,
              },
              event.at);
          ++revision_;
        }
        return {};
      },
      event.message);
}

RuntimeSelection RuntimeCoordinator::active(MonotonicTime now) {
  return RuntimeSelection{arbiter_.active(now), revision_};
}

std::expected<ContextKey, RuntimeError>
RuntimeCoordinator::activate(std::string_view instance_id,
                             std::optional<std::chrono::milliseconds> duration, MonotonicTime now) {
  const auto configured = std::ranges::find_if(
      configured_instances_, [instance_id](const auto &item) { return item.first == instance_id; });
  if (configured == configured_instances_.end()) {
    return std::unexpected(runtime_error(RuntimeErrorCode::unknown_instance,
                                         std::string{instance_id},
                                         "module instance does not exist"));
  }
  if (!configured->second) {
    return std::unexpected(runtime_error(RuntimeErrorCode::disabled_instance,
                                         std::string{instance_id}, "module instance is disabled"));
  }
  const std::optional<MonotonicTime> deadline =
      duration ? std::optional{now + *duration} : std::nullopt;
  auto activated = arbiter_.activate(instance_id, deadline, now);
  if (!activated) {
    return std::unexpected(runtime_error(RuntimeErrorCode::unavailable_instance,
                                         std::string{instance_id},
                                         "module instance has no available context"));
  }
  ++revision_;
  return *activated;
}

std::expected<ContextKey, RuntimeError>
RuntimeCoordinator::dismiss_active(std::string_view context_id, MonotonicTime now) {
  const PublishedContext *selected = arbiter_.active(now);
  if (selected == nullptr || selected->key.context_id != context_id) {
    return std::unexpected(
        runtime_error(RuntimeErrorCode::unknown_context, "", "active context does not match"));
  }
  const ContextKey key = selected->key;
  static_cast<void>(arbiter_.dismiss_active(context_id, now));
  ++revision_;
  return key;
}

std::vector<RuntimeModuleStatus> RuntimeCoordinator::module_statuses(MonotonicTime now) {
  std::vector<RuntimeModuleStatus> result;
  result.reserve(configured_instances_.size());
  for (const auto &[instance_id, enabled] : configured_instances_) {
    const auto state = module_states_.find(instance_id);
    result.push_back({.id = instance_id,
                      .enabled = enabled,
                      .state = state == module_states_.end() ? ModuleState::stopped : state->second,
                      .available = enabled && arbiter_.available(instance_id, now)});
  }
  return result;
}

void RuntimeCoordinator::reject(const ContextKey &key) {
  arbiter_.dismiss(key);
  ++revision_;
}

std::vector<VisibilityUpdate> RuntimeCoordinator::visibility_updates(MonotonicTime now,
                                                                     IslandMode mode) {
  const PublishedContext *selected = arbiter_.active(now);
  std::vector<VisibilityUpdate> updates;
  for (const auto &instance_id : enabled_instances_) {
    if (!ready_instances_.contains(instance_id)) {
      continue;
    }
    Visibility next = Visibility::hidden;
    if (selected != nullptr && selected->key.instance_id == instance_id) {
      next = mode == IslandMode::expanded && selected->expanded ? Visibility::expanded_active
                                                                : Visibility::compact_active;
    }
    const auto previous = visibility_.find(instance_id);
    if (previous == visibility_.end() || previous->second != next) {
      visibility_[instance_id] = next;
      updates.push_back(VisibilityUpdate{instance_id, next});
    }
  }
  return updates;
}

} // namespace gisland
