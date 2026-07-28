#include "gisland/config.hpp"

#include <toml++/toml.hpp>

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
    if (!command.has_value()) {
      return std::unexpected(command.error());
    }
    if (!enabled.has_value()) {
      return std::unexpected(enabled.error());
    }
    if (!options.has_value()) {
      return std::unexpected(options.error());
    }

    modules.push_back(
        ModuleInstanceConfig{std::move(*id), std::move(*command), *enabled, std::move(*options)});
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
