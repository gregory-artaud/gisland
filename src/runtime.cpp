#include "gisland/runtime.hpp"

#include <chrono>
#include <map>
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
  if (config.maximum_protocol >= ProtocolVersion{1, 2}) {
    capabilities.emplace_back("context-images");
  }
  if (config.maximum_protocol >= ProtocolVersion{1, 3}) {
    capabilities.emplace_back("rich-content");
  }
  if (config.maximum_protocol >= ProtocolVersion{1, 4}) {
    capabilities.emplace_back("independent-views");
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
              .minimum = config.minimum_protocol,
              .maximum = config.maximum_protocol,
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
    : arbiter_(config.compact_default.empty() ? config.default_module : config.compact_default,
               config.expanded_default.empty() ? config.default_module : config.expanded_default) {
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
          std::erase_if(pending_replacements_, [&typed_event](const auto &entry) {
            return entry.first.instance_id == typed_event.instance_id;
          });
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
          const ContextKey key{event.instance_id, message.context_id};
          remember_replacement(key, event.at);
          arbiter_.publish(
              PublishedContext{
                  .key = {event.instance_id, message.context_id},
                  .priority = message.priority,
                  .expires_at = message.expires_in ? std::optional{event.at + *message.expires_in}
                                                   : std::nullopt,
                  .compact = message.compact,
                  .expanded = message.expanded,
                  .resources = message.resources,
                  .presentation = message.presentation,
                  .revision = revision_ + 1,
              },
              event.at);
          ++revision_;
        } else if constexpr (std::is_same_v<Message, DismissMessage>) {
          const ContextKey key{event.instance_id, message.context_id};
          pending_replacements_.erase(key);
          arbiter_.dismiss(key);
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
          const ContextKey key{event.instance_id, std::string{configured_context_id}};
          remember_replacement(key, event.at);
          arbiter_.publish(
              PublishedContext{
                  .key = {event.instance_id, std::string{configured_context_id}},
                  .priority = 0,
                  .expires_at = std::nullopt,
                  .compact = instantiated.compact,
                  .expanded = instantiated.expanded,
                  .revision = revision_ + 1,
              },
              event.at);
          ++revision_;
        }
        return {};
      },
      event.message);
}

void RuntimeCoordinator::remember_replacement(const ContextKey &key, MonotonicTime now) {
  if (pending_replacements_.contains(key)) {
    return;
  }
  const auto *previous = arbiter_.find(key, now);
  pending_replacements_.emplace(
      key, previous == nullptr ? std::nullopt : std::optional<PublishedContext>{*previous});
}

RuntimeSelection RuntimeCoordinator::active(MonotonicTime now) {
  return active(ViewSlot::compact, now);
}

RuntimeSelection RuntimeCoordinator::active(ViewSlot slot, MonotonicTime now) {
  const PublishedContext *selected = arbiter_.active(slot, now);
  std::erase_if(pending_replacements_, [this, now](const auto &entry) {
    return arbiter_.find(entry.first, now) == nullptr;
  });
  const SceneNode *scene = nullptr;
  if (selected != nullptr) {
    const auto &contribution = slot == ViewSlot::compact ? selected->compact : selected->expanded;
    scene = contribution ? &*contribution : nullptr;
  }
  return RuntimeSelection{selected, selected == nullptr ? revision_ : selected->revision, scene};
}

RuntimeSelections RuntimeCoordinator::selections(MonotonicTime now) {
  return RuntimeSelections{active(ViewSlot::compact, now), active(ViewSlot::expanded, now)};
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
  return dismiss_active(context_id, ViewSlot::compact, now);
}

std::expected<ContextKey, RuntimeError>
RuntimeCoordinator::dismiss_active(std::string_view context_id, ViewSlot slot, MonotonicTime now) {
  const PublishedContext *selected = arbiter_.active(slot, now);
  if (selected == nullptr || selected->key.context_id != context_id) {
    return std::unexpected(
        runtime_error(RuntimeErrorCode::unknown_context, "", "active context does not match"));
  }
  ContextKey key = selected->key;
  arbiter_.dismiss(key);
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

std::expected<PreparedRuntimeReload, RuntimeError>
// Reload preflight intentionally stages every preserved and replaced runtime collection together.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
RuntimeCoordinator::prepare_reload(const ReloadPlan &plan) const {
  PreparedRuntimeReload prepared{
      .arbiter = arbiter_,
      .configured_instances = {},
      .module_states = {},
      .views = {},
      .enabled_instances = {},
      .ready_instances = ready_instances_,
      .visibility = visibility_,
      .revision = revision_ + 1,
  };
  prepared.configured_instances.reserve(plan.candidate.modules.size());
  prepared.enabled_instances.reserve(plan.candidate.modules.size());
  for (const auto &change : plan.changes) {
    if (change.kind == ModuleReloadKind::unchanged ||
        change.kind == ModuleReloadKind::view_updated) {
      continue;
    }
    prepared.arbiter.dismiss_instance(change.instance_id);
    prepared.ready_instances.erase(change.instance_id);
    prepared.visibility.erase(change.instance_id);
  }

  for (const auto &module : plan.candidate.modules) {
    prepared.configured_instances.emplace_back(module.id, module.enabled);
    if (!module.enabled) {
      continue;
    }
    prepared.enabled_instances.push_back(module.id);
    const auto change = std::ranges::find_if(
        plan.changes, [&module](const auto &item) { return item.instance_id == module.id; });
    const bool preserves_state =
        change != plan.changes.end() && (change->kind == ModuleReloadKind::unchanged ||
                                         change->kind == ModuleReloadKind::view_updated);
    const auto state = module_states_.find(module.id);
    prepared.module_states.emplace(module.id, preserves_state && state != module_states_.end()
                                                  ? state->second
                                                  : ModuleState::stopped);

    if (!module.view) {
      if (change != plan.changes.end() && change->kind == ModuleReloadKind::view_updated) {
        prepared.arbiter.dismiss({module.id, std::string{configured_context_id}});
      }
      continue;
    }
    if (change != plan.changes.end() && change->kind == ModuleReloadKind::unchanged) {
      const auto current_view = views_.find(module.id);
      if (current_view != views_.end()) {
        prepared.views.emplace(module.id, current_view->second);
      }
      continue;
    }

    ModuleViewState candidate_view{module.view->compact, module.view->expanded};
    if (change != plan.changes.end() && change->kind == ModuleReloadKind::view_updated) {
      const auto current_view = views_.find(module.id);
      if (current_view != views_.end() && current_view->second.snapshot()) {
        // Presence is established immediately above before copying into candidate state.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        auto applied = candidate_view.apply(*current_view->second.snapshot());
        if (!applied) {
          return std::unexpected(runtime_error(RuntimeErrorCode::invalid_snapshot, module.id,
                                               "snapshot failed at template '" +
                                                   applied.error().template_path + "' and data '" +
                                                   applied.error().data_path + "'"));
        }
      }
      if (const auto &views = candidate_view.views(); views) {
        prepared.arbiter.publish(
            PublishedContext{
                .key = {module.id, std::string{configured_context_id}},
                .priority = 0,
                .expires_at = std::nullopt,
                .compact = views->compact,
                .expanded = views->expanded,
                .revision = prepared.revision,
            },
            MonotonicTime{});
      } else {
        prepared.arbiter.dismiss({module.id, std::string{configured_context_id}});
      }
    }
    prepared.views.emplace(module.id, std::move(candidate_view));
  }
  prepared.arbiter.set_defaults(
      plan.candidate.compact_default.empty() ? plan.candidate.default_module
                                             : plan.candidate.compact_default,
      plan.candidate.expanded_default.empty() ? plan.candidate.default_module
                                              : plan.candidate.expanded_default);
  return prepared;
}

void RuntimeCoordinator::commit_reload(PreparedRuntimeReload prepared) noexcept {
  arbiter_ = std::move(prepared.arbiter);
  configured_instances_ = std::move(prepared.configured_instances);
  module_states_ = std::move(prepared.module_states);
  views_ = std::move(prepared.views);
  enabled_instances_ = std::move(prepared.enabled_instances);
  ready_instances_ = std::move(prepared.ready_instances);
  visibility_ = std::move(prepared.visibility);
  pending_replacements_.clear();
  revision_ = prepared.revision;
}

void RuntimeCoordinator::accept(const ContextKey &key) { pending_replacements_.erase(key); }

void RuntimeCoordinator::reject(const ContextKey &key, MonotonicTime now) {
  const ContextKey owned_key{key.instance_id, key.context_id};
  const auto pending = pending_replacements_.find(owned_key);
  std::optional<PublishedContext> replacement;
  if (pending != pending_replacements_.end()) {
    replacement = std::move(pending->second);
    pending_replacements_.erase(pending);
  }
  if (replacement.has_value()) {
    arbiter_.publish(std::move(replacement).value(), now);
  } else {
    arbiter_.dismiss(owned_key);
  }
  ++revision_;
}

std::vector<VisibilityUpdate> RuntimeCoordinator::visibility_updates(MonotonicTime now,
                                                                     IslandMode mode) {
  const PublishedContext *selected =
      arbiter_.active(mode == IslandMode::expanded ? ViewSlot::expanded : ViewSlot::compact, now);
  std::vector<VisibilityUpdate> updates;
  for (const auto &instance_id : enabled_instances_) {
    if (!ready_instances_.contains(instance_id)) {
      continue;
    }
    Visibility next = Visibility::hidden;
    if (selected != nullptr && selected->key.instance_id == instance_id) {
      next =
          mode == IslandMode::expanded ? Visibility::expanded_active : Visibility::compact_active;
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
