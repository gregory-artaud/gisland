#include "gisland/bootstrap.hpp"
#include "gisland/module_manifest.hpp"

#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace gisland {
namespace {

[[nodiscard]] BootstrapError bootstrap_error(BootstrapStage stage, std::filesystem::path path,
                                             std::string message) {
  return BootstrapError{stage, std::move(path), std::move(message)};
}

[[nodiscard]] std::expected<std::string, BootstrapError>
read_text(const std::filesystem::path &path, BootstrapStage stage) {
  std::ifstream stream{path};
  if (!stream) {
    return std::unexpected(bootstrap_error(stage, path, "could not open file"));
  }
  std::ostringstream content;
  content << stream.rdbuf();
  if (!stream.good() && !stream.eof()) {
    return std::unexpected(bootstrap_error(stage, path, "could not read file"));
  }
  return content.str();
}

[[nodiscard]] std::optional<std::string> environment_value(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::string{value};
}

} // namespace

std::filesystem::path resolve_distributed_data(const std::filesystem::path &executable,
                                               const std::filesystem::path &build_bindir,
                                               const std::filesystem::path &build_data,
                                               const std::filesystem::path &installed_data) {
  return executable.parent_path().lexically_normal() == build_bindir.lexically_normal()
             ? build_data
             : installed_data;
}

std::expected<RuntimeRoots, BootstrapError>
resolve_runtime_roots(std::optional<std::string> xdg_config_home, std::optional<std::string> home,
                      std::optional<std::string> xdg_data_home,
                      std::filesystem::path distributed_data) {
  std::filesystem::path config_home;
  if (xdg_config_home && !xdg_config_home->empty()) {
    config_home = *xdg_config_home;
  } else if (home && !home->empty()) {
    config_home = std::filesystem::path{*home} / ".config";
  } else {
    return std::unexpected(
        bootstrap_error(BootstrapStage::environment, {}, "XDG_CONFIG_HOME and HOME are unset"));
  }
  if (!config_home.is_absolute()) {
    return std::unexpected(bootstrap_error(BootstrapStage::environment, config_home,
                                           "configuration home must be absolute"));
  }
  std::filesystem::path data_home;
  if (xdg_data_home && !xdg_data_home->empty()) {
    data_home = *xdg_data_home;
  } else if (home && !home->empty()) {
    data_home = std::filesystem::path{*home} / ".local/share";
  } else {
    return std::unexpected(
        bootstrap_error(BootstrapStage::environment, {}, "XDG_DATA_HOME and HOME are unset"));
  }
  if (!data_home.is_absolute()) {
    return std::unexpected(
        bootstrap_error(BootstrapStage::environment, data_home, "data home must be absolute"));
  }
  return RuntimeRoots{std::move(config_home), std::move(data_home), std::move(distributed_data)};
}

std::expected<RuntimeBootstrap, BootstrapError> load_runtime_bootstrap(const RuntimeRoots &roots) {
  const auto user_config = roots.config_home / "gisland/config.toml";
  std::error_code filesystem_error;
  const auto config_path = std::filesystem::is_regular_file(user_config, filesystem_error)
                               ? user_config
                               : roots.distributed_data / "config.toml";
  return load_runtime_bootstrap(roots, config_path);
}

std::expected<RuntimeBootstrap, BootstrapError>
load_runtime_bootstrap(const RuntimeRoots &roots, const std::filesystem::path &config_path) {
  const auto catalog = discover_module_catalog(roots.data_home / "gisland/modules",
                                               roots.distributed_data / "modules");
  auto config = load_config(config_path, catalog);
  if (!config) {
    return std::unexpected(bootstrap_error(BootstrapStage::configuration, config_path,
                                           config.error().path + ": " + config.error().message));
  }

  const auto user_theme = roots.config_home / "gisland/themes" / (config->theme + ".toml");
  const auto distributed_theme = roots.distributed_data / "themes" / (config->theme + ".toml");
  std::error_code filesystem_error;
  const bool has_user_theme = std::filesystem::is_regular_file(user_theme, filesystem_error);
  const auto theme_path = has_user_theme ? user_theme : distributed_theme;
  const auto asset_root = has_user_theme ? roots.config_home / "gisland" : roots.distributed_data;
  auto theme_text = read_text(theme_path, BootstrapStage::theme);
  if (!theme_text) {
    return std::unexpected(theme_text.error());
  }
  auto theme = parse_theme(*theme_text, theme_path.string());
  if (!theme) {
    return std::unexpected(bootstrap_error(BootstrapStage::theme, theme_path,
                                           theme.error().path + ": " + theme.error().message));
  }
  std::vector<std::filesystem::path> manifest_paths;
  for (const auto &module : config->modules) {
    if (module.manifest_path) {
      manifest_paths.push_back(*module.manifest_path);
    }
  }
  return RuntimeBootstrap{
      .config = std::move(*config),
      .theme = std::move(*theme),
      .asset_root = asset_root,
      .config_path = config_path,
      .theme_path = theme_path,
      .manifest_paths = std::move(manifest_paths),
      .roots = roots,
  };
}

std::expected<RuntimeBootstrap, BootstrapError> load_runtime_bootstrap_from_environment() {
  std::error_code executable_error;
  const auto executable = std::filesystem::read_symlink("/proc/self/exe", executable_error);
  const auto distributed_data =
      executable_error ? std::filesystem::path{GISLAND_INSTALL_DATA_DIR}
                       : resolve_distributed_data(executable, GISLAND_BUILD_BINDIR,
                                                  GISLAND_BUILD_DATA_DIR, GISLAND_INSTALL_DATA_DIR);
  auto roots =
      resolve_runtime_roots(environment_value("XDG_CONFIG_HOME"), environment_value("HOME"),
                            environment_value("XDG_DATA_HOME"), distributed_data);
  if (!roots) {
    return std::unexpected(roots.error());
  }
  return load_runtime_bootstrap(*roots);
}

} // namespace gisland
