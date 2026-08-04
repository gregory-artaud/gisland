#include "gisland/bootstrap.hpp"

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

std::expected<RuntimeRoots, BootstrapError>
resolve_runtime_roots(std::optional<std::string> xdg_config_home, std::optional<std::string> home,
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
  return RuntimeRoots{std::move(config_home), std::move(distributed_data)};
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
  auto config = load_config(config_path);
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
  return RuntimeBootstrap{
      .config = std::move(*config),
      .theme = std::move(*theme),
      .asset_root = asset_root,
      .config_path = config_path,
      .theme_path = theme_path,
      .roots = roots,
  };
}

std::expected<RuntimeBootstrap, BootstrapError> load_runtime_bootstrap_from_environment() {
  auto roots = resolve_runtime_roots(environment_value("XDG_CONFIG_HOME"),
                                     environment_value("HOME"), GISLAND_DISTRIBUTED_DATA_DIR);
  if (!roots) {
    return std::unexpected(roots.error());
  }
  return load_runtime_bootstrap(*roots);
}

} // namespace gisland
