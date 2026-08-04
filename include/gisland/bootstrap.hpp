#pragma once

#include "gisland/config.hpp"
#include "gisland/theme.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gisland {

enum class BootstrapStage { environment, configuration, theme };

struct BootstrapError {
  BootstrapStage stage;
  std::filesystem::path path;
  std::string message;
};

struct RuntimeRoots {
  std::filesystem::path config_home;
  std::filesystem::path data_home;
  std::filesystem::path distributed_data;
};

struct RuntimeBootstrap {
  AppConfig config;
  Theme theme;
  std::filesystem::path asset_root;
  std::filesystem::path config_path;
  std::filesystem::path theme_path;
  std::vector<std::filesystem::path> manifest_paths;
  RuntimeRoots roots;
};

[[nodiscard]] std::filesystem::path resolve_distributed_data(
    const std::filesystem::path &executable, const std::filesystem::path &build_bindir,
    const std::filesystem::path &build_data, const std::filesystem::path &installed_data);

[[nodiscard]] std::expected<RuntimeRoots, BootstrapError>
resolve_runtime_roots(std::optional<std::string> xdg_config_home, std::optional<std::string> home,
                      std::optional<std::string> xdg_data_home,
                      std::filesystem::path distributed_data);
[[nodiscard]] std::expected<RuntimeBootstrap, BootstrapError>
load_runtime_bootstrap(const RuntimeRoots &roots);
[[nodiscard]] std::expected<RuntimeBootstrap, BootstrapError>
load_runtime_bootstrap(const RuntimeRoots &roots, const std::filesystem::path &config_path);
[[nodiscard]] std::expected<RuntimeBootstrap, BootstrapError>
load_runtime_bootstrap_from_environment();

} // namespace gisland
