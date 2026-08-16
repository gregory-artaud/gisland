#pragma once

#include "gisland/protocol.hpp"
#include "gisland/scene_template.hpp"

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

struct ModuleCatalog;

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

struct ModuleFileDependency {
  std::filesystem::path path;
  std::string fingerprint;
  std::uint64_t device{};
  std::uint64_t inode{};
  std::uint64_t size{};
  std::int64_t modified_seconds{};
  std::int64_t modified_nanoseconds{};

  bool operator==(const ModuleFileDependency &) const = default;
};

struct ModuleStaticDependencies {
  std::optional<ModuleFileDependency> manifest;
  std::optional<ModuleFileDependency> config;
  std::optional<ModuleFileDependency> view;
  std::optional<ModuleFileDependency> entry;
};

struct ModuleInstanceConfig {
  std::string id;
  std::string module_id;
  std::optional<std::filesystem::path> manifest_path;
  std::vector<std::string> command;
  ProtocolVersion minimum_protocol{1, 0};
  ProtocolVersion maximum_protocol{1, 8};
  bool enabled{true};
  ConfigValue::Table options;
  RestartPolicy restart{RestartPolicy::on_failure};
  ModuleTimings timings;
  std::map<std::string, std::string> environment;
  std::optional<std::filesystem::path> working_directory;
  struct View {
    SceneTemplate compact;
    std::optional<SceneTemplate> expanded;
  };
  std::optional<View> view;
  ModuleStaticDependencies dependencies;
};

struct InteractionConfig {
  double animation_speed{1.25};
  std::chrono::milliseconds hover_exit{120};
};

struct AppConfig {
  std::string monitor;
  std::string theme;
  std::string default_module;
  std::string compact_default{};
  std::string expanded_default{};
  InteractionConfig interaction;
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
[[nodiscard]] std::expected<AppConfig, ConfigError>
parse_config(std::string_view text, std::string_view source_name, const ModuleCatalog &catalog);

[[nodiscard]] std::expected<AppConfig, ConfigError> load_config(const std::filesystem::path &path);
[[nodiscard]] std::expected<AppConfig, ConfigError> load_config(const std::filesystem::path &path,
                                                                const ModuleCatalog &catalog);
[[nodiscard]] std::expected<ModuleViewConfig, ConfigError>
parse_module_view_config(std::string_view text, std::string_view source_name);
[[nodiscard]] bool module_file_dependency_is_current(const ModuleFileDependency &dependency);

} // namespace gisland
