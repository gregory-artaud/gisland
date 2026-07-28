#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace gisland {

struct ConfigValue {
  using Array = std::vector<ConfigValue>;
  using Table = std::map<std::string, ConfigValue>;
  using Value = std::variant<std::string, std::int64_t, double, bool, Array, Table>;

  Value value;
};

struct ModuleInstanceConfig {
  std::string id;
  std::vector<std::string> command;
  bool enabled;
  ConfigValue::Table options;
};

struct AppConfig {
  std::string monitor;
  std::string theme;
  std::string default_module;
  std::vector<ModuleInstanceConfig> modules;
};

struct ConfigError {
  std::string source;
  std::string path;
  std::string message;
  std::size_t line;
  std::size_t column;
};

[[nodiscard]] std::expected<AppConfig, ConfigError> parse_config(std::string_view text,
                                                                 std::string_view source_name);

[[nodiscard]] std::expected<AppConfig, ConfigError> load_config(const std::filesystem::path &path);

} // namespace gisland
