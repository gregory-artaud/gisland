#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
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

enum class RestartPolicy { always, on_failure, never };

struct ModuleTimings {
  std::chrono::milliseconds handshake{2000};
  std::chrono::milliseconds graceful_shutdown{1000};
  std::chrono::milliseconds terminate_grace{500};
  std::chrono::milliseconds initial_backoff{250};
  std::chrono::milliseconds maximum_backoff{30000};
  std::chrono::milliseconds healthy_reset{60000};
};

struct ModuleInstanceConfig {
  std::string id;
  std::vector<std::string> command;
  bool enabled{true};
  ConfigValue::Table options;
  RestartPolicy restart{RestartPolicy::on_failure};
  ModuleTimings timings;
  std::map<std::string, std::string> environment;
  std::optional<std::filesystem::path> working_directory;
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
