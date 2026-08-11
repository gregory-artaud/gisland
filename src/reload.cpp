#include "gisland/reload.hpp"

#include "gisland/runtime.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace gisland {
namespace {

[[nodiscard]] bool equal_config_value(const ConfigValue &left, const ConfigValue &right);
[[nodiscard]] bool equal_template(const SceneTemplate &left, const SceneTemplate &right);

[[nodiscard]] bool equal_config_table(const ConfigValue::Table &left,
                                      const ConfigValue::Table &right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (const auto &[key, left_value] : left) {
    const auto found = right.find(key);
    if (found == right.end() || !equal_config_value(left_value, found->second)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool equal_config_array(const ConfigValue::Array &left,
                                      const ConfigValue::Array &right) {
  return left.size() == right.size() && std::ranges::equal(left, right, equal_config_value);
}

bool equal_config_value(const ConfigValue &left, const ConfigValue &right) {
  if (left.value.index() != right.value.index()) {
    return false;
  }
  return std::visit(
      [&right](const auto &left_value) {
        using Value = std::decay_t<decltype(left_value)>;
        const auto &right_value = std::get<Value>(right.value);
        if constexpr (std::is_same_v<Value, ConfigValue::Array>) {
          return equal_config_array(left_value, right_value);
        } else if constexpr (std::is_same_v<Value, ConfigValue::Table>) {
          return equal_config_table(left_value, right_value);
        } else {
          return left_value == right_value;
        }
      },
      left.value);
}

template <typename Value>
[[nodiscard]] bool equal_template_value(const TemplateValue<Value> &left,
                                        const TemplateValue<Value> &right) {
  if (left.index() != right.index()) {
    return false;
  }
  if (const auto *literal = std::get_if<Value>(&left)) {
    return *literal == std::get<Value>(right);
  }
  return std::get<DataBinding>(left).path == std::get<DataBinding>(right).path;
}

[[nodiscard]] bool equal_template_pointer(const SceneTemplatePtr &left,
                                          const SceneTemplatePtr &right) {
  if (!left || !right) {
    return left == right;
  }
  return equal_template(*left, *right);
}

[[nodiscard]] bool equal_child(const TemplateChild &left, const TemplateChild &right) {
  if (left.index() != right.index()) {
    return false;
  }
  if (const auto *pointer = std::get_if<SceneTemplatePtr>(&left)) {
    return equal_template_pointer(*pointer, std::get<SceneTemplatePtr>(right));
  }
  const auto &left_repeat = std::get<TemplateRepeat>(left);
  const auto &right_repeat = std::get<TemplateRepeat>(right);
  return left_repeat.source.path == right_repeat.source.path &&
         left_repeat.alias == right_repeat.alias &&
         equal_template_pointer(left_repeat.body, right_repeat.body);
}

[[nodiscard]] bool equal_children(const std::vector<TemplateChild> &left,
                                  const std::vector<TemplateChild> &right) {
  return left.size() == right.size() && std::ranges::equal(left, right, equal_child);
}

bool equal_template(const SceneTemplate &left, const SceneTemplate &right) {
  if (left.value.index() != right.value.index()) {
    return false;
  }
  return std::visit(
      [&right](const auto &left_value) {
        using Value = std::decay_t<decltype(left_value)>;
        const auto &right_value = std::get<Value>(right.value);
        if constexpr (std::is_same_v<Value, TemplateText>) {
          return equal_template_value(left_value.value, right_value.value) &&
                 equal_template_value(left_value.role, right_value.role) &&
                 equal_template_value(left_value.truncation, right_value.truncation);
        } else if constexpr (std::is_same_v<Value, TemplateIcon>) {
          return equal_template_value(left_value.name, right_value.name) &&
                 equal_template_value(left_value.accessible_label, right_value.accessible_label);
        } else if constexpr (std::is_same_v<Value, TemplateRow> ||
                             std::is_same_v<Value, TemplateColumn>) {
          return equal_children(left_value.children, right_value.children) &&
                 equal_template_value(left_value.alignment, right_value.alignment) &&
                 equal_template_value(left_value.gap, right_value.gap);
        } else if constexpr (std::is_same_v<Value, TemplateSpacer>) {
          return equal_template_value(left_value.flexible, right_value.flexible) &&
                 equal_template_value(left_value.size_token, right_value.size_token);
        } else if constexpr (std::is_same_v<Value, TemplateProgress>) {
          return equal_template_value(left_value.value, right_value.value) &&
                 equal_template_value(left_value.label, right_value.label) &&
                 equal_template_value(left_value.state, right_value.state) &&
                 equal_template_value(left_value.shape, right_value.shape);
        } else if constexpr (std::is_same_v<Value, TemplateIndicator>) {
          return equal_template_value(left_value.state, right_value.state) &&
                 equal_template_value(left_value.accessible_label, right_value.accessible_label);
        } else {
          return equal_template_pointer(left_value.content, right_value.content) &&
                 left_value.action_id == right_value.action_id &&
                 equal_template_value(left_value.enabled, right_value.enabled) &&
                 equal_template_value(left_value.accessible_label, right_value.accessible_label);
        }
      },
      left.value);
}

[[nodiscard]] bool equal_view(const std::optional<ModuleInstanceConfig::View> &left,
                              const std::optional<ModuleInstanceConfig::View> &right) {
  if (left.has_value() != right.has_value()) {
    return false;
  }
  if (!left) {
    return true;
  }
  if (!equal_template(left->compact, right->compact) ||
      left->expanded.has_value() != right->expanded.has_value()) {
    return false;
  }
  return !left->expanded || equal_template(*left->expanded, *right->expanded);
}

[[nodiscard]] bool equal_timings(const ModuleTimings &left, const ModuleTimings &right) {
  return left.handshake == right.handshake && left.graceful_shutdown == right.graceful_shutdown &&
         left.terminate_grace == right.terminate_grace &&
         left.initial_backoff == right.initial_backoff &&
         left.maximum_backoff == right.maximum_backoff && left.healthy_reset == right.healthy_reset;
}

[[nodiscard]] bool equal_process_config(const ModuleInstanceConfig &left,
                                        const ModuleInstanceConfig &right) {
  return left.module_id == right.module_id && left.manifest_path == right.manifest_path &&
         left.command == right.command && left.minimum_protocol == right.minimum_protocol &&
         left.maximum_protocol == right.maximum_protocol &&
         equal_config_table(left.options, right.options) && left.restart == right.restart &&
         equal_timings(left.timings, right.timings) && left.environment == right.environment &&
         left.working_directory == right.working_directory;
}

} // namespace

std::expected<RuntimeBootstrap, BootstrapError>
load_reload_candidate(const RuntimeBootstrap &current) {
  return load_runtime_bootstrap(current.roots, current.config_path);
}

std::expected<ReloadPlan, ReloadPlanError> plan_reload(const AppConfig &current,
                                                       const AppConfig &candidate,
                                                       std::string locale, std::string timezone) {
  std::map<std::string, const ModuleInstanceConfig *, std::less<>> current_modules;
  for (const auto &module : current.modules) {
    current_modules.emplace(module.id, &module);
  }

  ReloadPlan plan{.candidate = candidate, .changes = {}, .start_requests = {}, .supervisor = {}};
  for (const auto &module : candidate.modules) {
    const auto old = current_modules.find(module.id);
    ModuleReloadKind kind = ModuleReloadKind::added;
    bool starts = module.enabled;
    if (old != current_modules.end()) {
      const ModuleInstanceConfig &previous = *old->second;
      current_modules.erase(old);
      if (!previous.enabled && module.enabled) {
        kind = ModuleReloadKind::enabled;
      } else if (previous.enabled && !module.enabled) {
        kind = ModuleReloadKind::disabled;
        starts = false;
      } else if (!equal_process_config(previous, module)) {
        kind = ModuleReloadKind::process_modified;
        starts = module.enabled;
      } else if (!equal_view(previous.view, module.view)) {
        kind = ModuleReloadKind::view_updated;
        starts = false;
      } else {
        kind = ModuleReloadKind::unchanged;
        starts = false;
      }
    }
    plan.changes.push_back({module.id, kind, starts});
    if (starts) {
      auto request = make_module_start_request(module, locale, timezone);
      plan.start_requests.push_back(request);
      plan.supervisor.start_or_replace.push_back(std::move(request));
    } else if (kind == ModuleReloadKind::disabled) {
      plan.supervisor.stop_instances.push_back(module.id);
    }
  }
  for (const auto &module : current.modules) {
    if (current_modules.contains(module.id)) {
      plan.changes.push_back({module.id, ModuleReloadKind::removed, false});
      plan.supervisor.stop_instances.push_back(module.id);
    }
  }
  return plan;
}

} // namespace gisland
