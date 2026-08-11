#include "gisland/module_manifest.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace gisland {
namespace {

constexpr std::size_t maximum_name_bytes = 128;
constexpr std::size_t maximum_description_bytes = 4096;
constexpr std::size_t maximum_command_arguments = 64;
constexpr std::size_t maximum_command_argument_bytes = 4096;

[[nodiscard]] ModuleManifestError error_at(const std::filesystem::path &source, std::string path,
                                           std::string message, const toml::node *node = nullptr) {
  ModuleManifestError error{source, std::move(path), std::move(message)};
  if (node != nullptr) {
    error.line = static_cast<std::size_t>(node->source().begin.line);
    error.column = static_cast<std::size_t>(node->source().begin.column);
  }
  return error;
}

[[nodiscard]] bool valid_id(std::string_view value) {
  return !value.empty() && value.size() <= 128 &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::islower(character) != 0 || std::isdigit(character) != 0 ||
                  character == '.' || character == '_' || character == '-';
         });
}

[[nodiscard]] std::expected<std::string, ModuleManifestError>
required_string(const toml::table &table, std::string_view key, const std::filesystem::path &source,
                std::size_t maximum_bytes) {
  const auto *node = table.get(key);
  if (node == nullptr) {
    return std::unexpected(error_at(source, std::string{key}, "missing required string"));
  }
  const auto value = node->value_exact<std::string>();
  if (!value || value->empty() || value->size() > maximum_bytes) {
    return std::unexpected(error_at(source, std::string{key}, "expected a non-empty string", node));
  }
  return *value;
}

[[nodiscard]] std::expected<ConfigValue, ModuleManifestError>
convert_value(const toml::node &node, const std::string &path, const std::filesystem::path &source);

[[nodiscard]] std::expected<ConfigValue::Table, ModuleManifestError>
convert_table(const toml::table &table, const std::string &path,
              const std::filesystem::path &source) {
  ConfigValue::Table values;
  for (const auto &[key, node] : table) {
    const auto child_path =
        path.empty() ? std::string{key.str()} : path + "." + std::string{key.str()};
    auto value = convert_value(node, child_path, source);
    if (!value) {
      return std::unexpected(value.error());
    }
    values.emplace(key.str(), std::move(*value));
  }
  return values;
}

[[nodiscard]] std::expected<ConfigValue, ModuleManifestError>
convert_value(const toml::node &node, const std::string &path,
              const std::filesystem::path &source) {
  if (const auto *table = node.as_table()) {
    auto value = convert_table(*table, path, source);
    if (!value) {
      return std::unexpected(value.error());
    }
    return ConfigValue{std::move(*value)};
  }
  if (const auto *array = node.as_array()) {
    ConfigValue::Array values;
    values.reserve(array->size());
    for (std::size_t index = 0; index < array->size(); ++index) {
      auto value = convert_value((*array)[index], path + "[" + std::to_string(index) + "]", source);
      if (!value) {
        return std::unexpected(value.error());
      }
      values.push_back(std::move(*value));
    }
    return ConfigValue{std::move(values)};
  }
  if (const auto value = node.value_exact<std::string>()) {
    return ConfigValue{*value};
  }
  if (const auto value = node.value_exact<std::int64_t>()) {
    return ConfigValue{*value};
  }
  if (const auto value = node.value_exact<double>()) {
    return ConfigValue{*value};
  }
  if (const auto value = node.value_exact<bool>()) {
    return ConfigValue{*value};
  }
  return std::unexpected(error_at(source, path, "unsupported option value", &node));
}

[[nodiscard]] std::expected<std::vector<std::string>, ModuleManifestError>
parse_command(const toml::table &root, const std::filesystem::path &source) {
  const auto *node = root.get("command");
  const auto *array = node == nullptr ? nullptr : node->as_array();
  if (array == nullptr || array->empty() || array->size() > maximum_command_arguments) {
    return std::unexpected(error_at(source, "command", "expected a non-empty string array", node));
  }
  std::vector<std::string> command;
  command.reserve(array->size());
  for (std::size_t index = 0; index < array->size(); ++index) {
    const auto value = (*array)[index].value_exact<std::string>();
    if (!value || value->empty() || value->size() > maximum_command_argument_bytes) {
      return std::unexpected(error_at(source, "command[" + std::to_string(index) + "]",
                                      "expected a non-empty string", &(*array)[index]));
    }
    command.push_back(*value);
  }
  return command;
}

[[nodiscard]] std::expected<ModuleOptionType, ModuleManifestError>
parse_option_type(const toml::node &node, const std::filesystem::path &source,
                  const std::string &path) {
  const auto value = node.value_exact<std::string>();
  if (!value) {
    return std::unexpected(error_at(source, path, "expected a string", &node));
  }
  if (*value == "string") {
    return ModuleOptionType::string;
  }
  if (*value == "integer") {
    return ModuleOptionType::integer;
  }
  if (*value == "number") {
    return ModuleOptionType::number;
  }
  if (*value == "boolean") {
    return ModuleOptionType::boolean;
  }
  if (*value == "array") {
    return ModuleOptionType::array;
  }
  if (*value == "table") {
    return ModuleOptionType::table;
  }
  return std::unexpected(error_at(source, path, "unknown option type", &node));
}

[[nodiscard]] std::expected<std::map<std::string, ModuleOptionSchema, std::less<>>,
                            ModuleManifestError>
// Manifest schemas are intentionally decoded in one strict boundary function.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
parse_schema(const toml::table &root, const std::filesystem::path &source) {
  std::map<std::string, ModuleOptionSchema, std::less<>> result;
  const auto *schema_node = root.get("options_schema");
  if (schema_node == nullptr) {
    return result;
  }
  const auto *schema = schema_node->as_table();
  if (schema == nullptr) {
    return std::unexpected(error_at(source, "options_schema", "expected a table", schema_node));
  }
  for (const auto &[key, node] : *schema) {
    const std::string base = "options_schema." + std::string{key.str()};
    const auto *entry = node.as_table();
    if (entry == nullptr || entry->get("type") == nullptr) {
      return std::unexpected(error_at(source, base, "expected a schema table with type", &node));
    }
    for (const auto &[entry_key, entry_node] : *entry) {
      if (entry_key != "type" && entry_key != "required" && entry_key != "allowed") {
        return std::unexpected(error_at(source, base + "." + std::string{entry_key.str()},
                                        "unknown schema property", &entry_node));
      }
    }
    auto type = parse_option_type(*entry->get("type"), source, base + ".type");
    if (!type) {
      return std::unexpected(type.error());
    }
    ModuleOptionSchema parsed{.type = *type, .required = false, .allowed = {}};
    if (const auto *required = entry->get("required"); required != nullptr) {
      const auto value = required->value_exact<bool>();
      if (!value) {
        return std::unexpected(
            error_at(source, base + ".required", "expected a boolean", required));
      }
      parsed.required = *value;
    }
    if (const auto *allowed = entry->get("allowed"); allowed != nullptr) {
      const auto *array = allowed->as_array();
      if (parsed.type != ModuleOptionType::string || array == nullptr) {
        return std::unexpected(error_at(source, base + ".allowed",
                                        "allowed requires a string option and array", allowed));
      }
      for (std::size_t index = 0; index < array->size(); ++index) {
        const auto value = (*array)[index].value_exact<std::string>();
        if (!value) {
          return std::unexpected(error_at(source, base + ".allowed[" + std::to_string(index) + "]",
                                          "expected a string", &(*array)[index]));
        }
        parsed.allowed.push_back(*value);
      }
    }
    result.emplace(key.str(), std::move(parsed));
  }
  return result;
}

void discover_root(ModuleCatalog &catalog, const std::filesystem::path &root,
                   std::set<std::string, std::less<>> &claimed) {
  std::error_code error;
  if (!std::filesystem::is_directory(root, error)) {
    return;
  }
  std::vector<std::filesystem::path> directories;
  for (std::filesystem::directory_iterator iterator{root, error};
       !error && iterator != std::filesystem::directory_iterator{}; iterator.increment(error)) {
    if (iterator->is_directory(error)) {
      directories.push_back(iterator->path());
    }
  }
  std::ranges::sort(directories);
  for (const auto &directory : directories) {
    const auto id = directory.filename().string();
    if (!claimed.insert(id).second) {
      continue;
    }
    auto manifest = load_module_manifest(directory / "module.toml", id);
    if (manifest) {
      catalog.manifests.emplace(id, std::move(*manifest));
    } else {
      catalog.errors.emplace(id, std::move(manifest.error()));
    }
  }
}

} // namespace

bool option_matches_schema(const ConfigValue &value, const ModuleOptionSchema &schema) {
  const bool type_matches = std::visit(
      [&schema](const auto &typed) {
        using Type = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Type, std::string>) {
          return schema.type == ModuleOptionType::string;
        } else if constexpr (std::is_same_v<Type, std::int64_t>) {
          return schema.type == ModuleOptionType::integer ||
                 schema.type == ModuleOptionType::number;
        } else if constexpr (std::is_same_v<Type, double>) {
          return schema.type == ModuleOptionType::number;
        } else if constexpr (std::is_same_v<Type, bool>) {
          return schema.type == ModuleOptionType::boolean;
        } else if constexpr (std::is_same_v<Type, ConfigValue::Array>) {
          return schema.type == ModuleOptionType::array;
        } else {
          return schema.type == ModuleOptionType::table;
        }
      },
      value.value);
  if (!type_matches || schema.allowed.empty()) {
    return type_matches;
  }
  const auto *text = std::get_if<std::string>(&value.value);
  return text != nullptr && std::ranges::find(schema.allowed, *text) != schema.allowed.end();
}

std::expected<ModuleManifest, ModuleManifestError>
// Manifest parsing keeps validation adjacent to each external field.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
parse_module_manifest(std::string_view text, const std::filesystem::path &source,
                      std::string_view directory_id) {
  try {
    const auto root = toml::parse(text, source.string());
    for (const auto &[key, node] : root) {
      if (key != "id" && key != "name" && key != "description" && key != "command" &&
          key != "protocol" && key != "defaults" && key != "options_schema") {
        return std::unexpected(
            error_at(source, std::string{key.str()}, "unknown manifest property", &node));
      }
    }
    auto id = required_string(root, "id", source, 128);
    auto name = required_string(root, "name", source, maximum_name_bytes);
    auto command = parse_command(root, source);
    if (!id) {
      return std::unexpected(id.error());
    }
    if (!valid_id(*id) || *id != directory_id) {
      return std::unexpected(
          error_at(source, "id", "manifest ID must match its directory", root.get("id")));
    }
    if (!name) {
      return std::unexpected(name.error());
    }
    if (!command) {
      return std::unexpected(command.error());
    }
    std::string description;
    if (const auto *node = root.get("description"); node != nullptr) {
      const auto value = node->value_exact<std::string>();
      if (!value || value->size() > maximum_description_bytes) {
        return std::unexpected(error_at(source, "description", "expected a string", node));
      }
      description = *value;
    }
    const auto *protocol_node = root.get("protocol");
    const auto *protocol = protocol_node == nullptr ? nullptr : protocol_node->as_table();
    if (protocol == nullptr) {
      return std::unexpected(
          error_at(source, "protocol", "expected a protocol table", protocol_node));
    }
    for (const auto &[key, node] : *protocol) {
      if (key != "major" && key != "minimum_minor" && key != "maximum_minor") {
        return std::unexpected(error_at(source, "protocol." + std::string{key.str()},
                                        "unknown protocol property", &node));
      }
    }
    const auto major = protocol->get("major") == nullptr
                           ? std::optional<std::int64_t>{}
                           : protocol->get("major")->value_exact<std::int64_t>();
    const auto minimum = protocol->get("minimum_minor") == nullptr
                             ? std::optional<std::int64_t>{}
                             : protocol->get("minimum_minor")->value_exact<std::int64_t>();
    const auto maximum = protocol->get("maximum_minor") == nullptr
                             ? std::optional<std::int64_t>{}
                             : protocol->get("maximum_minor")->value_exact<std::int64_t>();
    constexpr auto maximum_protocol_component = std::numeric_limits<int>::max();
    if (!major || !minimum || !maximum || *major < 0 || *minimum < 0 || *maximum < *minimum ||
        *major > maximum_protocol_component || *minimum > maximum_protocol_component ||
        *maximum > maximum_protocol_component) {
      return std::unexpected(error_at(source, "protocol", "invalid protocol range", protocol_node));
    }
    auto schema = parse_schema(root, source);
    if (!schema) {
      return std::unexpected(schema.error());
    }
    ConfigValue::Table defaults;
    if (const auto *defaults_node = root.get("defaults"); defaults_node != nullptr) {
      const auto *table = defaults_node->as_table();
      if (table == nullptr) {
        return std::unexpected(error_at(source, "defaults", "expected a table", defaults_node));
      }
      auto converted = convert_table(*table, "defaults", source);
      if (!converted) {
        return std::unexpected(converted.error());
      }
      defaults = std::move(*converted);
    }
    for (const auto &[key, value] : defaults) {
      const auto found = schema->find(key);
      if (found == schema->end() || !option_matches_schema(value, found->second)) {
        return std::unexpected(error_at(source, "defaults." + key,
                                        "default is absent from or incompatible with the schema"));
      }
    }
    return ModuleManifest{std::move(*id),
                          std::move(*name),
                          std::move(description),
                          std::move(*command),
                          {static_cast<int>(*major), static_cast<int>(*minimum)},
                          {static_cast<int>(*major), static_cast<int>(*maximum)},
                          std::move(defaults),
                          std::move(*schema),
                          source};
  } catch (const toml::parse_error &error) {
    return std::unexpected(
        ModuleManifestError{source, "", std::string{error.description()},
                            static_cast<std::size_t>(error.source().begin.line),
                            static_cast<std::size_t>(error.source().begin.column)});
  }
}

std::expected<ModuleManifest, ModuleManifestError>
load_module_manifest(const std::filesystem::path &path, std::string_view directory_id) {
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    return std::unexpected(ModuleManifestError{path, "", "unable to open module manifest"});
  }
  const std::string contents{std::istreambuf_iterator<char>{stream},
                             std::istreambuf_iterator<char>{}};
  if (stream.bad()) {
    return std::unexpected(ModuleManifestError{path, "", "unable to read module manifest"});
  }
  return parse_module_manifest(contents, path, directory_id);
}

ModuleCatalog discover_module_catalog(const std::filesystem::path &config_modules,
                                      const std::filesystem::path &user_modules,
                                      const std::filesystem::path &distributed_modules) {
  ModuleCatalog catalog;
  std::set<std::string, std::less<>> claimed;
  discover_root(catalog, config_modules, claimed);
  discover_root(catalog, user_modules, claimed);
  discover_root(catalog, distributed_modules, claimed);
  return catalog;
}

} // namespace gisland
