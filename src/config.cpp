#include "gisland/config.hpp"

#include <toml++/toml.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace gisland {
namespace {

[[nodiscard]] ConfigError error_at(std::string_view source_name, std::string path,
                                   std::string message, const toml::node *node = nullptr) {
  std::size_t line = 0;
  std::size_t column = 0;
  if (node != nullptr) {
    line = static_cast<std::size_t>(node->source().begin.line);
    column = static_cast<std::size_t>(node->source().begin.column);
  }
  return ConfigError{std::string{source_name}, std::move(path), std::move(message), line, column};
}

[[nodiscard]] std::expected<std::string, ConfigError>
required_non_empty_string(const toml::table &table, std::string_view key, std::string path,
                          std::string_view source_name) {
  const auto *node = table.get(key);
  if (node == nullptr) {
    return std::unexpected(error_at(source_name, std::move(path), "missing required string"));
  }
  const auto value = node->value_exact<std::string>();
  if (!value.has_value()) {
    return std::unexpected(error_at(source_name, std::move(path), "expected a string", node));
  }
  if (value->empty()) {
    return std::unexpected(error_at(source_name, std::move(path), "value must not be empty", node));
  }
  return *value;
}

[[nodiscard]] std::expected<ConfigValue, ConfigError>
convert_option(const toml::node &node, const std::string &path, std::string_view source_name);

[[nodiscard]] std::expected<ConfigValue::Table, ConfigError>
convert_option_table(const toml::table &table, const std::string &path,
                     std::string_view source_name) {
  ConfigValue::Table result;
  for (const auto &[key, node] : table) {
    const std::string key_string{key.str()};
    std::string option_path{path};
    option_path += '.';
    option_path += key_string;
    auto value = convert_option(node, option_path, source_name);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    result.emplace(key_string, std::move(*value));
  }
  return result;
}

[[nodiscard]] std::expected<ConfigValue::Array, ConfigError>
convert_option_array(const toml::array &array, const std::string &path,
                     std::string_view source_name) {
  ConfigValue::Array result;
  result.reserve(array.size());
  for (std::size_t index = 0; index < array.size(); ++index) {
    auto value =
        convert_option(array[index], path + "[" + std::to_string(index) + "]", source_name);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    result.push_back(std::move(*value));
  }
  return result;
}

[[nodiscard]] std::expected<ConfigValue, ConfigError>
convert_option(const toml::node &node, const std::string &path, std::string_view source_name) {
  switch (node.type()) {
  case toml::node_type::table: {
    auto value = convert_option_table(*node.as_table(), path, source_name);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    return ConfigValue{std::move(*value)};
  }
  case toml::node_type::array: {
    auto value = convert_option_array(*node.as_array(), path, source_name);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    return ConfigValue{std::move(*value)};
  }
  case toml::node_type::string:
    return ConfigValue{node.ref<std::string>()};
  case toml::node_type::integer:
    return ConfigValue{node.ref<std::int64_t>()};
  case toml::node_type::floating_point:
    return ConfigValue{node.ref<double>()};
  case toml::node_type::boolean:
    return ConfigValue{node.ref<bool>()};
  case toml::node_type::date:
  case toml::node_type::time:
  case toml::node_type::date_time:
  case toml::node_type::none:
    return std::unexpected(error_at(source_name, path, "unsupported module option value", &node));
  }
  return std::unexpected(error_at(source_name, path, "unsupported module option value", &node));
}

[[nodiscard]] std::expected<std::vector<std::string>, ConfigError>
parse_command(const toml::table &table, std::size_t module_index, std::string_view source_name) {
  const std::string path = "modules[" + std::to_string(module_index) + "].command";
  const auto *node = table.get("command");
  if (node == nullptr) {
    return std::unexpected(error_at(source_name, path, "missing required command"));
  }
  const auto *array = node->as_array();
  if (array == nullptr) {
    return std::unexpected(error_at(source_name, path, "expected an array", node));
  }
  if (array->empty()) {
    return std::unexpected(error_at(source_name, path, "command must not be empty", node));
  }

  std::vector<std::string> command;
  command.reserve(array->size());
  for (std::size_t index = 0; index < array->size(); ++index) {
    const auto value = (*array)[index].value_exact<std::string>();
    if (!value.has_value()) {
      return std::unexpected(error_at(source_name, path + "[" + std::to_string(index) + "]",
                                      "expected a string", &(*array)[index]));
    }
    command.push_back(*value);
  }
  if (command.front().empty()) {
    return std::unexpected(
        error_at(source_name, path + "[0]", "executable must not be empty", &(*array)[0]));
  }
  return command;
}

[[nodiscard]] std::expected<bool, ConfigError>
parse_enabled(const toml::table &table, std::size_t module_index, std::string_view source_name) {
  const auto *node = table.get("enabled");
  if (node == nullptr) {
    return true;
  }
  const auto value = node->value_exact<bool>();
  if (!value.has_value()) {
    return std::unexpected(error_at(source_name,
                                    "modules[" + std::to_string(module_index) + "].enabled",
                                    "expected a boolean", node));
  }
  return *value;
}

[[nodiscard]] std::expected<ConfigValue::Table, ConfigError>
parse_options(const toml::table &table, std::size_t module_index, std::string_view source_name) {
  const auto *node = table.get("options");
  if (node == nullptr) {
    return ConfigValue::Table{};
  }
  const auto *options = node->as_table();
  const std::string path = "modules[" + std::to_string(module_index) + "].options";
  if (options == nullptr) {
    return std::unexpected(error_at(source_name, path, "expected a table", node));
  }
  return convert_option_table(*options, path, source_name);
}

[[nodiscard]] std::expected<RestartPolicy, ConfigError>
parse_restart_policy(const toml::table &table, std::size_t module_index,
                     std::string_view source_name) {
  const auto *node = table.get("restart");
  if (node == nullptr) {
    return RestartPolicy::on_failure;
  }
  const auto value = node->value_exact<std::string>();
  const std::string path = "modules[" + std::to_string(module_index) + "].restart";
  if (!value.has_value()) {
    return std::unexpected(error_at(source_name, path, "expected a string", node));
  }
  if (*value == "always") {
    return RestartPolicy::always;
  }
  if (*value == "on-failure") {
    return RestartPolicy::on_failure;
  }
  if (*value == "never") {
    return RestartPolicy::never;
  }
  return std::unexpected(
      error_at(source_name, path, R"(expected one of "always", "on-failure", or "never")", node));
}

[[nodiscard]] std::expected<std::chrono::milliseconds, ConfigError>
parse_duration(const toml::table &table, std::string_view key,
               std::chrono::milliseconds default_value, const std::string &path,
               std::string_view source_name) {
  constexpr auto maximum_duration = std::chrono::hours{24};
  const auto *node = table.get(key);
  if (node == nullptr) {
    return default_value;
  }
  const auto value = node->value_exact<std::int64_t>();
  if (!value.has_value()) {
    return std::unexpected(error_at(source_name, path, "expected integer milliseconds", node));
  }
  if (*value <= 0) {
    return std::unexpected(error_at(source_name, path, "duration must be positive", node));
  }
  const auto duration = std::chrono::milliseconds{*value};
  if (duration > maximum_duration) {
    return std::unexpected(error_at(source_name, path, "duration exceeds 24 hours", node));
  }
  return duration;
}

[[nodiscard]] std::expected<ModuleTimings, ConfigError>
parse_timings(const toml::table &table, std::size_t module_index, std::string_view source_name) {
  ModuleTimings timings;
  const auto *node = table.get("timings");
  if (node == nullptr) {
    return timings;
  }
  const auto *timing_table = node->as_table();
  const std::string base_path = "modules[" + std::to_string(module_index) + "].timings";
  if (timing_table == nullptr) {
    return std::unexpected(error_at(source_name, base_path, "expected a table", node));
  }

  auto handshake = parse_duration(*timing_table, "handshake_ms", timings.handshake,
                                  base_path + ".handshake_ms", source_name);
  auto graceful = parse_duration(*timing_table, "graceful_shutdown_ms", timings.graceful_shutdown,
                                 base_path + ".graceful_shutdown_ms", source_name);
  auto terminate = parse_duration(*timing_table, "terminate_grace_ms", timings.terminate_grace,
                                  base_path + ".terminate_grace_ms", source_name);
  auto initial = parse_duration(*timing_table, "initial_backoff_ms", timings.initial_backoff,
                                base_path + ".initial_backoff_ms", source_name);
  auto maximum = parse_duration(*timing_table, "maximum_backoff_ms", timings.maximum_backoff,
                                base_path + ".maximum_backoff_ms", source_name);
  auto healthy = parse_duration(*timing_table, "healthy_reset_ms", timings.healthy_reset,
                                base_path + ".healthy_reset_ms", source_name);
  if (!handshake.has_value()) {
    return std::unexpected(handshake.error());
  }
  if (!graceful.has_value()) {
    return std::unexpected(graceful.error());
  }
  if (!terminate.has_value()) {
    return std::unexpected(terminate.error());
  }
  if (!initial.has_value()) {
    return std::unexpected(initial.error());
  }
  if (!maximum.has_value()) {
    return std::unexpected(maximum.error());
  }
  if (!healthy.has_value()) {
    return std::unexpected(healthy.error());
  }
  if (*initial > *maximum) {
    return std::unexpected(error_at(source_name, base_path + ".initial_backoff_ms",
                                    "initial backoff must not exceed maximum backoff",
                                    timing_table->get("initial_backoff_ms")));
  }

  timings.handshake = *handshake;
  timings.graceful_shutdown = *graceful;
  timings.terminate_grace = *terminate;
  timings.initial_backoff = *initial;
  timings.maximum_backoff = *maximum;
  timings.healthy_reset = *healthy;
  return timings;
}

[[nodiscard]] std::expected<std::map<std::string, std::string>, ConfigError>
parse_environment(const toml::table &table, std::size_t module_index,
                  std::string_view source_name) {
  std::map<std::string, std::string> environment;
  const auto *node = table.get("environment");
  if (node == nullptr) {
    return environment;
  }
  const auto *environment_table = node->as_table();
  const std::string base_path = "modules[" + std::to_string(module_index) + "].environment";
  if (environment_table == nullptr) {
    return std::unexpected(error_at(source_name, base_path, "expected a table", node));
  }
  for (const auto &[key, value_node] : *environment_table) {
    const std::string key_string{key.str()};
    std::string path = base_path;
    path.push_back('.');
    path.append(key_string);
    if (key_string.empty() || key_string.contains('=')) {
      return std::unexpected(error_at(source_name, path, "invalid environment key", &value_node));
    }
    const auto value = value_node.value_exact<std::string>();
    if (!value.has_value()) {
      return std::unexpected(
          error_at(source_name, path, "expected an environment string", &value_node));
    }
    environment.emplace(key_string, *value);
  }
  return environment;
}

[[nodiscard]] std::expected<std::optional<std::filesystem::path>, ConfigError>
parse_working_directory(const toml::table &table, std::size_t module_index,
                        std::string_view source_name) {
  const auto *node = table.get("working_directory");
  if (node == nullptr) {
    return std::optional<std::filesystem::path>{};
  }
  const std::string path = "modules[" + std::to_string(module_index) + "].working_directory";
  const auto value = node->value_exact<std::string>();
  if (!value.has_value()) {
    return std::unexpected(error_at(source_name, path, "expected a string", node));
  }
  std::filesystem::path working_directory{*value};
  if (!working_directory.is_absolute()) {
    return std::unexpected(error_at(source_name, path, "working directory must be absolute", node));
  }
  return std::optional<std::filesystem::path>{std::move(working_directory)};
}

[[nodiscard]] bool valid_binding_path(std::string_view path) {
  return !path.empty() && path.front() != '.' && path.back() != '.' &&
         path.find("..") == std::string_view::npos;
}

[[nodiscard]] std::expected<void, ConfigError>
require_keys(const toml::table &table, const std::set<std::string_view> &allowed,
             const std::string &path, std::string_view source_name) {
  for (const auto &[key, node] : table) {
    if (!allowed.contains(key.str())) {
      return std::unexpected(error_at(source_name, path + "." + std::string{key.str()},
                                      "unknown template property", &node));
    }
  }
  return {};
}

template <typename T>
[[nodiscard]] std::expected<TemplateValue<T>, ConfigError>
parse_template_value(const toml::node &node, const std::string &path,
                     std::string_view source_name) {
  if (const auto *binding = node.as_table(); binding != nullptr) {
    if (binding->size() != 1 || !binding->contains("bind")) {
      return std::unexpected(
          error_at(source_name, path, "expected a literal or one bind property", &node));
    }
    const auto *bind_node = binding->get("bind");
    const auto value = bind_node->value_exact<std::string>();
    if (!value.has_value() || !valid_binding_path(*value)) {
      return std::unexpected(error_at(source_name, path + ".bind", "invalid binding path",
                                      bind_node));
    }
    return TemplateValue<T>{DataBinding{*value}};
  }

  if constexpr (std::is_same_v<T, double>) {
    if (const auto floating = node.value_exact<double>(); floating.has_value()) {
      return TemplateValue<T>{*floating};
    }
    if (const auto integer = node.value_exact<std::int64_t>(); integer.has_value()) {
      return TemplateValue<T>{static_cast<double>(*integer)};
    }
  } else if (const auto literal = node.value_exact<T>(); literal.has_value()) {
    return TemplateValue<T>{*literal};
  }
  return std::unexpected(error_at(source_name, path, "template value has the wrong type", &node));
}

template <typename T>
[[nodiscard]] std::expected<TemplateValue<T>, ConfigError>
template_field(const toml::table &table, std::string_view key, const std::string &path,
               std::string_view source_name, std::optional<T> default_value = std::nullopt) {
  const auto *node = table.get(key);
  if (node == nullptr) {
    if (default_value.has_value()) {
      return TemplateValue<T>{std::move(*default_value)};
    }
    return std::unexpected(error_at(source_name, path, "missing required template value"));
  }
  return parse_template_value<T>(*node, path, source_name);
}

[[nodiscard]] std::expected<SceneTemplate, ConfigError>
parse_scene_template(const toml::table &table, const std::string &path,
                     std::string_view source_name);

[[nodiscard]] std::expected<std::vector<TemplateChild>, ConfigError>
parse_template_children(const toml::table &table, const std::string &path,
                        std::string_view source_name) {
  const auto *node = table.get("children");
  if (node == nullptr || node->as_array() == nullptr) {
    return std::unexpected(error_at(source_name, path + ".children", "expected a child array",
                                    node));
  }
  std::vector<TemplateChild> children;
  const auto &array = *node->as_array();
  children.reserve(array.size());
  for (std::size_t index = 0; index < array.size(); ++index) {
    const auto item_path = path + ".children[" + std::to_string(index) + "]";
    const auto *child = array[index].as_table();
    if (child == nullptr) {
      return std::unexpected(
          error_at(source_name, item_path, "expected a template table", &array[index]));
    }
    if (child->contains("repeat")) {
      auto keys = require_keys(*child, {"repeat", "as", "template"}, item_path, source_name);
      if (!keys) {
        return std::unexpected(keys.error());
      }
      auto source = required_non_empty_string(*child, "repeat", item_path + ".repeat", source_name);
      auto alias = required_non_empty_string(*child, "as", item_path + ".as", source_name);
      const auto *body_node = child->get("template");
      const auto *body = body_node == nullptr ? nullptr : body_node->as_table();
      if (!source) {
        return std::unexpected(source.error());
      }
      if (!valid_binding_path(*source)) {
        return std::unexpected(error_at(source_name, item_path + ".repeat",
                                        "invalid binding path", child->get("repeat")));
      }
      if (!alias || alias->find('.') != std::string::npos) {
        return std::unexpected(alias ? error_at(source_name, item_path + ".as", "invalid alias",
                                                 child->get("as"))
                                     : alias.error());
      }
      if (body == nullptr) {
        return std::unexpected(error_at(source_name, item_path + ".template",
                                        "expected a template table", body_node));
      }
      auto parsed = parse_scene_template(*body, item_path + ".template", source_name);
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      children.emplace_back(
          TemplateRepeat{DataBinding{std::move(*source)}, std::move(*alias),
                         std::make_shared<const SceneTemplate>(std::move(*parsed))});
      continue;
    }
    auto parsed = parse_scene_template(*child, item_path, source_name);
    if (!parsed) {
      return std::unexpected(parsed.error());
    }
    children.emplace_back(std::make_shared<const SceneTemplate>(std::move(*parsed)));
  }
  return children;
}

[[nodiscard]] std::expected<SceneTemplate, ConfigError>
parse_scene_template(const toml::table &table, const std::string &path,
                     std::string_view source_name) {
  if (table.contains("repeat")) {
    return std::unexpected(error_at(source_name, path, "repeat is only allowed in child arrays",
                                    table.get("repeat")));
  }
  auto type = required_non_empty_string(table, "type", path + ".type", source_name);
  if (!type) {
    return std::unexpected(type.error());
  }
  if (*type == "text") {
    auto keys = require_keys(table, {"type", "value", "role", "truncation"}, path, source_name);
    auto value = template_field<std::string>(table, "value", path + ".value", source_name);
    auto role = template_field<std::string>(table, "role", path + ".role", source_name);
    auto truncation = template_field<std::string>(table, "truncation", path + ".truncation",
                                                  source_name, std::string{"end"});
    if (!keys) return std::unexpected(keys.error());
    if (!value) return std::unexpected(value.error());
    if (!role) return std::unexpected(role.error());
    if (!truncation) return std::unexpected(truncation.error());
    return SceneTemplate{TemplateText{std::move(*value), std::move(*role),
                                      std::move(*truncation)}};
  }
  if (*type == "icon") {
    auto keys = require_keys(table, {"type", "name", "accessible_label"}, path, source_name);
    auto name = template_field<std::string>(table, "name", path + ".name", source_name);
    auto label = template_field<std::string>(table, "accessible_label",
                                             path + ".accessible_label", source_name,
                                             std::string{});
    if (!keys) return std::unexpected(keys.error());
    if (!name) return std::unexpected(name.error());
    if (!label) return std::unexpected(label.error());
    return SceneTemplate{TemplateIcon{std::move(*name), std::move(*label)}};
  }
  if (*type == "spacer") {
    auto keys = require_keys(table, {"type", "flexible", "size_token"}, path, source_name);
    auto flexible = template_field<bool>(table, "flexible", path + ".flexible", source_name, true);
    auto size = template_field<std::string>(table, "size_token", path + ".size_token", source_name,
                                            std::string{});
    if (!keys) return std::unexpected(keys.error());
    if (!flexible) return std::unexpected(flexible.error());
    if (!size) return std::unexpected(size.error());
    return SceneTemplate{TemplateSpacer{std::move(*flexible), std::move(*size)}};
  }
  if (*type == "progress") {
    auto keys = require_keys(table, {"type", "value", "label", "state"}, path, source_name);
    auto value = template_field<double>(table, "value", path + ".value", source_name);
    auto label = template_field<std::string>(table, "label", path + ".label", source_name,
                                             std::string{});
    auto state = template_field<std::string>(table, "state", path + ".state", source_name,
                                             std::string{});
    if (!keys) return std::unexpected(keys.error());
    if (!value) return std::unexpected(value.error());
    if (!label) return std::unexpected(label.error());
    if (!state) return std::unexpected(state.error());
    return SceneTemplate{TemplateProgress{std::move(*value), std::move(*label), std::move(*state)}};
  }
  if (*type == "row" || *type == "column") {
    auto keys = require_keys(table, {"type", "children", "alignment", "gap"}, path, source_name);
    auto children = parse_template_children(table, path, source_name);
    auto alignment = template_field<std::string>(table, "alignment", path + ".alignment",
                                                 source_name, std::string{"center"});
    auto gap = template_field<std::string>(table, "gap", path + ".gap", source_name,
                                           std::string{"normal"});
    if (!keys) return std::unexpected(keys.error());
    if (!children) return std::unexpected(children.error());
    if (!alignment) return std::unexpected(alignment.error());
    if (!gap) return std::unexpected(gap.error());
    if (*type == "row") {
      return SceneTemplate{TemplateRow{std::move(*children), std::move(*alignment),
                                       std::move(*gap)}};
    }
    return SceneTemplate{TemplateColumn{std::move(*children), std::move(*alignment),
                                        std::move(*gap)}};
  }
  if (*type == "button") {
    auto keys = require_keys(table, {"type", "content", "action_id", "enabled",
                                     "accessible_label"}, path, source_name);
    const auto *content_node = table.get("content");
    const auto *content = content_node == nullptr ? nullptr : content_node->as_table();
    auto action = required_non_empty_string(table, "action_id", path + ".action_id", source_name);
    auto enabled = template_field<bool>(table, "enabled", path + ".enabled", source_name, true);
    auto label = template_field<std::string>(table, "accessible_label",
                                             path + ".accessible_label", source_name,
                                             std::string{});
    if (!keys) return std::unexpected(keys.error());
    if (content == nullptr) {
      return std::unexpected(error_at(source_name, path + ".content", "expected a template table",
                                      content_node));
    }
    auto parsed_content = parse_scene_template(*content, path + ".content", source_name);
    if (!parsed_content) return std::unexpected(parsed_content.error());
    if (!action) return std::unexpected(action.error());
    if (!enabled) return std::unexpected(enabled.error());
    if (!label) return std::unexpected(label.error());
    return SceneTemplate{TemplateButton{std::make_shared<const SceneTemplate>(
                                            std::move(*parsed_content)),
                                        std::move(*action), std::move(*enabled),
                                        std::move(*label)}};
  }
  return std::unexpected(error_at(source_name, path + ".type", "unknown template node type",
                                  table.get("type")));
}

[[nodiscard]] std::expected<std::optional<ModuleInstanceConfig::View>, ConfigError>
parse_module_view(const toml::table &module, std::size_t index, std::string_view source_name) {
  const auto *node = module.get("view");
  if (node == nullptr) {
    return std::optional<ModuleInstanceConfig::View>{};
  }
  const auto *view = node->as_table();
  const auto path = "modules[" + std::to_string(index) + "].view";
  if (view == nullptr) {
    return std::unexpected(error_at(source_name, path, "expected a view table", node));
  }
  auto keys = require_keys(*view, {"compact", "expanded"}, path, source_name);
  if (!keys) return std::unexpected(keys.error());
  const auto *compact_node = view->get("compact");
  const auto *compact = compact_node == nullptr ? nullptr : compact_node->as_table();
  if (compact == nullptr) {
    return std::unexpected(error_at(source_name, path + ".compact",
                                    "expected a compact template table", compact_node));
  }
  auto parsed_compact = parse_scene_template(*compact, path + ".compact", source_name);
  if (!parsed_compact) return std::unexpected(parsed_compact.error());
  std::optional<SceneTemplate> parsed_expanded;
  if (const auto *expanded_node = view->get("expanded"); expanded_node != nullptr) {
    const auto *expanded = expanded_node->as_table();
    if (expanded == nullptr) {
      return std::unexpected(error_at(source_name, path + ".expanded",
                                      "expected an expanded template table", expanded_node));
    }
    auto candidate = parse_scene_template(*expanded, path + ".expanded", source_name);
    if (!candidate) return std::unexpected(candidate.error());
    parsed_expanded = std::move(*candidate);
  }
  return std::optional<ModuleInstanceConfig::View>{ModuleInstanceConfig::View{
      std::move(*parsed_compact), std::move(parsed_expanded)}};
}

[[nodiscard]] std::expected<std::vector<ModuleInstanceConfig>, ConfigError>
parse_modules(const toml::table &root, std::string_view source_name) {
  const auto *node = root.get("modules");
  if (node == nullptr) {
    return std::unexpected(error_at(source_name, "modules", "missing required module list"));
  }
  const auto *array = node->as_array();
  if (array == nullptr) {
    return std::unexpected(error_at(source_name, "modules", "expected an array of tables", node));
  }

  std::set<std::string> ids;
  std::vector<ModuleInstanceConfig> modules;
  modules.reserve(array->size());
  for (std::size_t index = 0; index < array->size(); ++index) {
    const auto *module_table = (*array)[index].as_table();
    const std::string base_path = "modules[" + std::to_string(index) + "]";
    if (module_table == nullptr) {
      return std::unexpected(
          error_at(source_name, base_path, "expected a module table", &(*array)[index]));
    }

    auto id = required_non_empty_string(*module_table, "id", base_path + ".id", source_name);
    if (!id.has_value()) {
      return std::unexpected(id.error());
    }
    if (!ids.insert(*id).second) {
      return std::unexpected(error_at(source_name, base_path + ".id",
                                      "module instance ID must be unique",
                                      module_table->get("id")));
    }

    auto command = parse_command(*module_table, index, source_name);
    auto enabled = parse_enabled(*module_table, index, source_name);
    auto options = parse_options(*module_table, index, source_name);
    auto restart = parse_restart_policy(*module_table, index, source_name);
    auto timings = parse_timings(*module_table, index, source_name);
    auto environment = parse_environment(*module_table, index, source_name);
    auto working_directory = parse_working_directory(*module_table, index, source_name);
    auto view = parse_module_view(*module_table, index, source_name);
    if (!command.has_value()) {
      return std::unexpected(command.error());
    }
    if (!enabled.has_value()) {
      return std::unexpected(enabled.error());
    }
    if (!options.has_value()) {
      return std::unexpected(options.error());
    }
    if (!restart.has_value()) {
      return std::unexpected(restart.error());
    }
    if (!timings.has_value()) {
      return std::unexpected(timings.error());
    }
    if (!environment.has_value()) {
      return std::unexpected(environment.error());
    }
    if (!working_directory.has_value()) {
      return std::unexpected(working_directory.error());
    }
    if (!view.has_value()) {
      return std::unexpected(view.error());
    }

    modules.push_back(ModuleInstanceConfig{
        .id = std::move(*id),
        .command = std::move(*command),
        .enabled = *enabled,
        .options = std::move(*options),
        .restart = *restart,
        .timings = *timings,
        .environment = std::move(*environment),
        .working_directory = std::move(*working_directory),
        .view = std::move(*view),
    });
  }
  return modules;
}

[[nodiscard]] std::expected<AppConfig, ConfigError> parse_table(const toml::table &root,
                                                                std::string_view source_name) {
  auto monitor = required_non_empty_string(root, "monitor", "monitor", source_name);
  auto theme = required_non_empty_string(root, "theme", "theme", source_name);
  auto default_module =
      required_non_empty_string(root, "default_module", "default_module", source_name);
  if (!monitor.has_value()) {
    return std::unexpected(monitor.error());
  }
  if (!theme.has_value()) {
    return std::unexpected(theme.error());
  }
  if (!default_module.has_value()) {
    return std::unexpected(default_module.error());
  }

  auto modules = parse_modules(root, source_name);
  if (!modules.has_value()) {
    return std::unexpected(modules.error());
  }

  bool default_is_enabled = false;
  for (const auto &module : *modules) {
    if (module.id == *default_module && module.enabled) {
      default_is_enabled = true;
      break;
    }
  }
  if (!default_is_enabled) {
    return std::unexpected(error_at(source_name, "default_module",
                                    "default module must reference an enabled instance",
                                    root.get("default_module")));
  }

  return AppConfig{std::move(*monitor), std::move(*theme), std::move(*default_module),
                   std::move(*modules)};
}

} // namespace

std::expected<AppConfig, ConfigError> parse_config(std::string_view text,
                                                   std::string_view source_name) {
  try {
    const auto root = toml::parse(text, source_name);
    return parse_table(root, source_name);
  } catch (const toml::parse_error &error) {
    return std::unexpected(ConfigError{
        std::string{source_name},
        "",
        std::string{error.description()},
        static_cast<std::size_t>(error.source().begin.line),
        static_cast<std::size_t>(error.source().begin.column),
    });
  }
}

std::expected<AppConfig, ConfigError> load_config(const std::filesystem::path &path) {
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    return std::unexpected(
        ConfigError{path.string(), "", "unable to open configuration file", 0, 0});
  }

  const std::string contents{std::istreambuf_iterator<char>{stream},
                             std::istreambuf_iterator<char>{}};
  if (stream.bad()) {
    return std::unexpected(
        ConfigError{path.string(), "", "unable to read configuration file", 0, 0});
  }
  return parse_config(contents, path.string());
}

} // namespace gisland
