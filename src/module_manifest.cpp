#include "gisland/module_manifest.hpp"
#include "gisland/content_fingerprint.hpp"

#include <toml++/toml.hpp>

#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
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

class FileDescriptor {
public:
  explicit FileDescriptor(int value = -1) : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      ::close(value_);
    }
  }
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  FileDescriptor(FileDescriptor &&other) noexcept : value_(std::exchange(other.value_, -1)) {}
  [[nodiscard]] int get() const { return value_; }

private:
  int value_;
};

struct OpenedPackageFile {
  std::filesystem::path path;
  std::string contents;
  struct stat metadata{};
};

[[nodiscard]] std::expected<OpenedPackageFile, ModuleManifestError>
open_package_file(const std::filesystem::path &package_directory,
                  const std::filesystem::path &relative, const std::filesystem::path &source,
                  std::string_view key) {
  FileDescriptor package_fd{
      ::open(package_directory.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (package_fd.get() < 0) {
    return std::unexpected(
        ModuleManifestError{source, std::string{key}, "unable to open module package directory"});
  }
  const std::string relative_text = relative.string();
  const open_how how{
      .flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW,
      .mode = 0,
      .resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS,
  };
  FileDescriptor file_fd{static_cast<int>(
      ::syscall(SYS_openat2, package_fd.get(), relative_text.c_str(), &how, sizeof(how)))};
  if (file_fd.get() < 0) {
    return std::unexpected(ModuleManifestError{
        source, std::string{key}, "path must name a regular file within the module package"});
  }
  struct stat metadata{};
  if (::fstat(file_fd.get(), &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
    return std::unexpected(ModuleManifestError{
        source, std::string{key}, "path must name a regular file within the module package"});
  }
  std::string contents;
  std::array<char, 64 * 1024> buffer{};
  while (true) {
    const auto count = ::read(file_fd.get(), buffer.data(), buffer.size());
    if (count > 0) {
      contents.append(buffer.data(), static_cast<std::size_t>(count));
      continue;
    }
    if (count == 0) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    return std::unexpected(
        ModuleManifestError{source, std::string{key}, "unable to read package file"});
  }
  return OpenedPackageFile{package_directory / relative, std::move(contents), metadata};
}

[[nodiscard]] ModuleFileDependency dependency(const OpenedPackageFile &file) {
  return ModuleFileDependency{
      .path = file.path,
      .fingerprint = content_fingerprint(file.contents),
      .device = static_cast<std::uint64_t>(file.metadata.st_dev),
      .inode = static_cast<std::uint64_t>(file.metadata.st_ino),
      .size = static_cast<std::uint64_t>(file.metadata.st_size),
      .modified_seconds = file.metadata.st_mtim.tv_sec,
      .modified_nanoseconds = file.metadata.st_mtim.tv_nsec,
  };
}

[[nodiscard]] ModuleManifestError manifest_error(const ConfigError &error) {
  return ModuleManifestError{error.source, error.path, error.message, error.line, error.column};
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

[[nodiscard]] std::expected<std::optional<OpenedPackageFile>, ModuleManifestError>
resolve_package_file(const toml::table &root, std::string_view key,
                     const std::filesystem::path &source,
                     const std::optional<std::filesystem::path> &package_directory) {
  const auto *node = root.get(key);
  if (node == nullptr) {
    return std::optional<OpenedPackageFile>{};
  }
  const auto value = node->value_exact<std::string>();
  if (!value || value->empty()) {
    return std::unexpected(
        error_at(source, std::string{key}, "expected a non-empty package-relative path", node));
  }
  const std::filesystem::path relative{*value};
  if (relative.is_absolute() ||
      std::ranges::find(relative, std::filesystem::path{".."}) != relative.end()) {
    return std::unexpected(
        error_at(source, std::string{key}, "path must remain within the module package", node));
  }
  if (!package_directory) {
    return std::unexpected(error_at(source, std::string{key},
                                    "package path requires loading from a manifest file", node));
  }
  if (package_directory->string().find_first_of(";?") != std::string::npos) {
    return std::unexpected(error_at(source, std::string{key},
                                    "module package path contains Lua path metacharacters", node));
  }

  std::error_code filesystem_error;
  const auto canonical_package = std::filesystem::canonical(*package_directory, filesystem_error);
  if (filesystem_error) {
    return std::unexpected(
        error_at(source, std::string{key}, "unable to resolve module package directory", node));
  }
  auto opened = open_package_file(canonical_package, relative, source, key);
  if (!opened) {
    return std::unexpected(error_at(source, std::string{key}, opened.error().message, node));
  }
  return std::optional<OpenedPackageFile>{std::move(*opened)};
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
      if (entry_key != "type" && entry_key != "required" && entry_key != "allowed" &&
          entry_key != "minimum" && entry_key != "maximum") {
        return std::unexpected(error_at(source, base + "." + std::string{entry_key.str()},
                                        "unknown schema property", &entry_node));
      }
    }
    auto type = parse_option_type(*entry->get("type"), source, base + ".type");
    if (!type) {
      return std::unexpected(type.error());
    }
    ModuleOptionSchema parsed{.type = *type,
                              .required = false,
                              .allowed = {},
                              .minimum = std::nullopt,
                              .maximum = std::nullopt};
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
    const auto parse_bound = [&](std::string_view property)
        -> std::expected<std::optional<ModuleOptionSchema::NumericBound>, ModuleManifestError> {
      const auto *bound = entry->get(property);
      if (bound == nullptr) {
        return std::optional<ModuleOptionSchema::NumericBound>{};
      }
      const auto bound_path = base + "." + std::string{property};
      if (parsed.type == ModuleOptionType::integer) {
        const auto value = bound->value_exact<std::int64_t>();
        if (!value) {
          return std::unexpected(
              error_at(source, bound_path, "integer bounds require an integer", bound));
        }
        return std::optional<ModuleOptionSchema::NumericBound>{*value};
      }
      if (parsed.type != ModuleOptionType::number) {
        return std::unexpected(
            error_at(source, bound_path, "bounds require an integer or number option", bound));
      }
      if (const auto value = bound->value_exact<std::int64_t>()) {
        return std::optional<ModuleOptionSchema::NumericBound>{*value};
      }
      const auto value = bound->value_exact<double>();
      if (!value || !std::isfinite(*value)) {
        return std::unexpected(error_at(source, bound_path, "expected a finite number", bound));
      }
      return std::optional<ModuleOptionSchema::NumericBound>{*value};
    };
    auto minimum = parse_bound("minimum");
    auto maximum = parse_bound("maximum");
    if (!minimum) {
      return std::unexpected(minimum.error());
    }
    if (!maximum) {
      return std::unexpected(maximum.error());
    }
    parsed.minimum = std::move(*minimum);
    parsed.maximum = std::move(*maximum);
    const auto bound_value = [](const ModuleOptionSchema::NumericBound &bound) {
      return std::visit([](const auto value) { return static_cast<long double>(value); }, bound);
    };
    if (parsed.minimum && parsed.maximum &&
        bound_value(*parsed.minimum) > bound_value(*parsed.maximum)) {
      return std::unexpected(error_at(source, base + ".minimum", "minimum must not exceed maximum",
                                      entry->get("minimum")));
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
          return schema.type == ModuleOptionType::number && std::isfinite(typed);
        } else if constexpr (std::is_same_v<Type, bool>) {
          return schema.type == ModuleOptionType::boolean;
        } else if constexpr (std::is_same_v<Type, ConfigValue::Array>) {
          return schema.type == ModuleOptionType::array;
        } else {
          return schema.type == ModuleOptionType::table;
        }
      },
      value.value);
  if (!type_matches) {
    return false;
  }
  if (!schema.allowed.empty()) {
    const auto *text = std::get_if<std::string>(&value.value);
    if (text == nullptr || std::ranges::find(schema.allowed, *text) == schema.allowed.end()) {
      return false;
    }
  }
  if (!schema.minimum && !schema.maximum) {
    return true;
  }
  const auto numeric_value = std::visit(
      [](const auto &typed) -> std::optional<long double> {
        using Type = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Type, std::int64_t> || std::is_same_v<Type, double>) {
          if constexpr (std::is_same_v<Type, double>) {
            if (!std::isfinite(typed)) {
              return std::nullopt;
            }
          }
          return static_cast<long double>(typed);
        }
        return std::nullopt;
      },
      value.value);
  if (!numeric_value) {
    return false;
  }
  const auto bound_value = [](const ModuleOptionSchema::NumericBound &bound) {
    return std::visit([](const auto typed) { return static_cast<long double>(typed); }, bound);
  };
  return (!schema.minimum || *numeric_value >= bound_value(*schema.minimum)) &&
         (!schema.maximum || *numeric_value <= bound_value(*schema.maximum));
}

std::expected<ModuleManifest, ModuleManifestError>
// Manifest parsing keeps validation adjacent to each external field.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
parse_module_manifest_impl(std::string_view text, const std::filesystem::path &source,
                           std::string_view directory_id,
                           const std::optional<std::filesystem::path> &package_directory) {
  try {
    const auto root = toml::parse(text, source.string());
    for (const auto &[key, node] : root) {
      if (key != "id" && key != "name" && key != "description" && key != "command" &&
          key != "entry" && key != "config" && key != "view" && key != "protocol" &&
          key != "defaults" && key != "options_schema") {
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
    const bool is_lua_host = std::filesystem::path{command->front()}.filename() ==
                             std::filesystem::path{"gisland-lua-host"};
    if (is_lua_host && !root.contains("entry")) {
      return std::unexpected(error_at(source, "entry", "Lua host manifests require an entry"));
    }
    if (!is_lua_host && root.contains("entry")) {
      return std::unexpected(
          error_at(source, "entry", "entry is valid only for gisland-lua-host", root.get("entry")));
    }
    auto entry_path = resolve_package_file(root, "entry", source, package_directory);
    auto config_path = resolve_package_file(root, "config", source, package_directory);
    auto view_path = resolve_package_file(root, "view", source, package_directory);
    if (!entry_path) {
      return std::unexpected(entry_path.error());
    }
    if (!config_path) {
      return std::unexpected(config_path.error());
    }
    if (!view_path) {
      return std::unexpected(view_path.error());
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
    const auto validate_defaults = [&](const ConfigValue::Table &values, const toml::table &table,
                                       const std::filesystem::path &defaults_source)
        -> std::expected<void, ModuleManifestError> {
      for (const auto &[key, value] : values) {
        const auto found = schema->find(key);
        if (found == schema->end() || !option_matches_schema(value, found->second)) {
          return std::unexpected(error_at(defaults_source, "defaults." + key,
                                          "default is absent from or incompatible with the schema",
                                          table.get(key)));
        }
      }
      return {};
    };
    if (*config_path && root.contains("defaults")) {
      return std::unexpected(error_at(source, "defaults",
                                      "inline defaults cannot be combined with config",
                                      root.get("defaults")));
    }
    ConfigValue::Table defaults;
    std::optional<ModuleFileDependency> config_dependency;
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
      auto validated = validate_defaults(defaults, *table, source);
      if (!validated) {
        return std::unexpected(validated.error());
      }
    }
    if (*config_path) {
      try {
        const auto config_root =
            toml::parse((*config_path)->contents, (*config_path)->path.string());
        for (const auto &[key, node] : config_root) {
          if (key != "defaults") {
            return std::unexpected(error_at((*config_path)->path, std::string{key.str()},
                                            "unknown package config property", &node));
          }
        }
        const auto *defaults_node = config_root.get("defaults");
        const auto *table = defaults_node == nullptr ? nullptr : defaults_node->as_table();
        if (table == nullptr) {
          return std::unexpected(error_at((*config_path)->path, "defaults",
                                          "expected one defaults table", defaults_node));
        }
        auto converted = convert_table(*table, "defaults", (*config_path)->path);
        if (!converted) {
          return std::unexpected(converted.error());
        }
        defaults = std::move(*converted);
        auto validated = validate_defaults(defaults, *table, (*config_path)->path);
        if (!validated) {
          return std::unexpected(validated.error());
        }
      } catch (const toml::parse_error &error) {
        return std::unexpected(
            ModuleManifestError{(*config_path)->path, "", std::string{error.description()},
                                static_cast<std::size_t>(error.source().begin.line),
                                static_cast<std::size_t>(error.source().begin.column)});
      }
      config_dependency = dependency(**config_path);
    }
    std::optional<ModuleViewConfig> view;
    std::optional<ModuleFileDependency> view_dependency;
    if (*view_path) {
      auto parsed_view =
          parse_module_view_config((*view_path)->contents, (*view_path)->path.string());
      if (!parsed_view) {
        return std::unexpected(manifest_error(parsed_view.error()));
      }
      view = std::move(*parsed_view);
      view_dependency = dependency(**view_path);
    }
    return ModuleManifest{
        std::move(*id),
        std::move(*name),
        std::move(description),
        std::move(*command),
        {static_cast<int>(*major), static_cast<int>(*minimum)},
        {static_cast<int>(*major), static_cast<int>(*maximum)},
        std::move(defaults),
        std::move(*schema),
        source,
        *entry_path ? std::optional<std::filesystem::path>{(*entry_path)->path} : std::nullopt,
        *config_path ? std::optional<std::filesystem::path>{(*config_path)->path} : std::nullopt,
        *view_path ? std::optional<std::filesystem::path>{(*view_path)->path} : std::nullopt,
        std::move(view),
        ModuleStaticDependencies{
            .manifest =
                ModuleFileDependency{.path = source, .fingerprint = content_fingerprint(text)},
            .config = std::move(config_dependency),
            .view = std::move(view_dependency),
            .entry = *entry_path ? std::optional<ModuleFileDependency>{dependency(**entry_path)}
                                 : std::nullopt,
        }};
  } catch (const toml::parse_error &error) {
    return std::unexpected(
        ModuleManifestError{source, "", std::string{error.description()},
                            static_cast<std::size_t>(error.source().begin.line),
                            static_cast<std::size_t>(error.source().begin.column)});
  }
}

std::expected<ModuleManifest, ModuleManifestError>
parse_module_manifest(std::string_view text, const std::filesystem::path &source,
                      std::string_view directory_id) {
  return parse_module_manifest_impl(text, source, directory_id, std::nullopt);
}

std::expected<ModuleManifest, ModuleManifestError>
load_module_manifest(const std::filesystem::path &path, std::string_view directory_id) {
  auto opened = open_package_file(path.parent_path(), path.filename(), path, "");
  if (!opened) {
    return std::unexpected(ModuleManifestError{path, "", "unable to open module manifest"});
  }
  auto parsed =
      parse_module_manifest_impl(opened->contents, path, directory_id, path.parent_path());
  if (parsed) {
    parsed->dependencies.manifest = dependency(*opened);
  }
  return parsed;
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

bool module_file_dependency_is_current(const ModuleFileDependency &expected) {
  if (expected.device == 0) {
    return true;
  }
  const auto package = expected.path.parent_path();
  auto opened = open_package_file(package, expected.path.filename(), expected.path, "");
  if (!opened) {
    return false;
  }
  const auto actual = dependency(*opened);
  const bool identity_matches =
      expected.device == 0 ||
      (actual.device == expected.device && actual.inode == expected.inode &&
       actual.size == expected.size && actual.modified_seconds == expected.modified_seconds &&
       actual.modified_nanoseconds == expected.modified_nanoseconds);
  return actual.fingerprint == expected.fingerprint && identity_matches;
}

} // namespace gisland
