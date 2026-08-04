#pragma once

#include "gisland/config.hpp"
#include "gisland/protocol.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace gisland {

enum class ModuleOptionType { string, integer, number, boolean, array, table };

struct ModuleOptionSchema {
  ModuleOptionType type;
  bool required{false};
  std::vector<std::string> allowed;
};

struct ModuleManifest {
  std::string id;
  std::string name;
  std::string description;
  std::vector<std::string> command;
  ProtocolVersion minimum_protocol;
  ProtocolVersion maximum_protocol;
  ConfigValue::Table defaults;
  std::map<std::string, ModuleOptionSchema, std::less<>> options_schema;
  std::filesystem::path path;
};

struct ModuleManifestError {
  std::filesystem::path source;
  std::string path;
  std::string message;
  std::size_t line{0};
  std::size_t column{0};
};

struct ModuleCatalog {
  std::map<std::string, ModuleManifest, std::less<>> manifests;
  std::map<std::string, ModuleManifestError, std::less<>> errors;
};

[[nodiscard]] std::expected<ModuleManifest, ModuleManifestError>
parse_module_manifest(std::string_view text, const std::filesystem::path &source,
                      std::string_view directory_id);
[[nodiscard]] std::expected<ModuleManifest, ModuleManifestError>
load_module_manifest(const std::filesystem::path &path, std::string_view directory_id);
[[nodiscard]] ModuleCatalog
discover_module_catalog(const std::filesystem::path &user_modules,
                        const std::filesystem::path &distributed_modules);
[[nodiscard]] bool option_matches_schema(const ConfigValue &value,
                                         const ModuleOptionSchema &schema);

} // namespace gisland
