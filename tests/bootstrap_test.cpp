#include "gisland/bootstrap.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ =
        std::filesystem::temp_directory_path() / ("gisland-bootstrap-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path &path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream{path};
  if (!stream) {
    throw std::runtime_error{"could not create bootstrap fixture"};
  }
  stream << content;
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream stream{path};
  std::ostringstream content;
  content << stream.rdbuf();
  return content.str();
}

constexpr std::string_view config_text = R"(
monitor = "primary"
theme = "default"
default_module = "clock"

[[modules]]
id = "clock"
command = ["/bin/true"]
)";

} // namespace

TEST_CASE("runtime roots follow XDG config home and HOME fallback") {
  const auto explicit_roots =
      gisland::resolve_runtime_roots(std::string{"/tmp/custom-config"}, std::string{"/home/user"},
                                     std::string{"/tmp/custom-data"}, "/opt/gisland");
  REQUIRE(explicit_roots.has_value());
  CHECK(explicit_roots->config_home == "/tmp/custom-config");
  CHECK(explicit_roots->data_home == "/tmp/custom-data");
  CHECK(explicit_roots->distributed_data == "/opt/gisland");

  const auto fallback_roots = gisland::resolve_runtime_roots(
      std::nullopt, std::string{"/home/user"}, std::nullopt, "/usr/share/gisland");
  REQUIRE(fallback_roots.has_value());
  CHECK(fallback_roots->config_home == "/home/user/.config");
  CHECK(fallback_roots->data_home == "/home/user/.local/share");

  const auto missing_home = gisland::resolve_runtime_roots(std::nullopt, std::nullopt, std::nullopt,
                                                           "/usr/share/gisland");
  REQUIRE_FALSE(missing_home.has_value());
  CHECK(missing_home.error().stage == gisland::BootstrapStage::environment);
}

TEST_CASE("distributed resources follow build and installed executable locations") {
  CHECK(gisland::resolve_distributed_data(
            "/work/build/dev/gisland", "/work/build/dev", "/work/build/dev/assets",
            "/opt/share/gisland/distributed") == "/work/build/dev/assets");
  CHECK(gisland::resolve_distributed_data(
            "/opt/bin/gisland", "/work/build/dev", "/work/build/dev/assets",
            "/opt/share/gisland/distributed") == "/opt/share/gisland/distributed");
}

TEST_CASE("bootstrap loads config and distributed theme before graphical startup") {
  TemporaryDirectory temporary;
  const auto config_home = temporary.path() / "config";
  write_file(config_home / "gisland/config.toml", config_text);
  const gisland::RuntimeRoots roots{config_home, temporary.path() / "data",
                                    GISLAND_TEST_ASSET_ROOT};

  const auto bootstrap = gisland::load_runtime_bootstrap(roots);
  REQUIRE(bootstrap.has_value());
  CHECK(bootstrap->config.default_module == "clock");
  CHECK(bootstrap->theme_path ==
        std::filesystem::path{GISLAND_TEST_ASSET_ROOT} / "themes/default.toml");
  CHECK(bootstrap->asset_root == std::filesystem::path{GISLAND_TEST_ASSET_ROOT});
  CHECK(bootstrap->theme.views().compact.padding_horizontal == 14.0);
  CHECK(bootstrap->theme.views().compact.padding_vertical == 4.0);
  CHECK(bootstrap->theme.views().compact.radius == 16.0);
  CHECK(bootstrap->theme.views().compact.min_width == 230.0);
  CHECK(bootstrap->theme.views().compact.min_height == 32.0);
  CHECK(bootstrap->theme.views().compact.max_height == 32.0);
  CHECK(bootstrap->theme.views().compact.border == 0.0);
  CHECK(bootstrap->theme.views().expanded.min_height == 96.0);
  CHECK(bootstrap->theme.views().expanded.border == 0.0);
  CHECK(bootstrap->theme.typography().at("compact-primary").size == 12.0);
  CHECK(bootstrap->theme.typography().at("compact-secondary").size == 12.0);
  REQUIRE(std::holds_alternative<std::string>(bootstrap->theme.buttons().background));
  REQUIRE(std::holds_alternative<std::string>(bootstrap->theme.buttons().disabled_background));
  CHECK(std::get<std::string>(bootstrap->theme.buttons().background) == "surface");
  CHECK(std::get<std::string>(bootstrap->theme.buttons().disabled_background) == "surface");
}

TEST_CASE("bootstrap gives a valid user theme priority over the distributed theme") {
  TemporaryDirectory temporary;
  const auto config_home = temporary.path() / "config";
  write_file(config_home / "gisland/config.toml", config_text);
  std::string user_theme =
      read_file(std::filesystem::path{GISLAND_TEST_ASSET_ROOT} / "themes/default.toml");
  const auto accent = user_theme.find("#7C5CFC");
  REQUIRE(accent != std::string::npos);
  user_theme.replace(accent, 7, "#112233");
  write_file(config_home / "gisland/themes/default.toml", user_theme);

  const auto bootstrap = gisland::load_runtime_bootstrap(
      gisland::RuntimeRoots{config_home, temporary.path() / "data", GISLAND_TEST_ASSET_ROOT});
  REQUIRE(bootstrap.has_value());
  CHECK(bootstrap->theme_path == config_home / "gisland/themes/default.toml");
  CHECK(bootstrap->asset_root == config_home / "gisland");
  CHECK(bootstrap->theme.palette().at("accent") == gisland::Rgba{0x11, 0x22, 0x33, 0xFF});
}

TEST_CASE(
    "bootstrap uses the distributed clock-calendar configuration when user config is absent") {
  TemporaryDirectory temporary;
  const auto bootstrap = gisland::load_runtime_bootstrap(gisland::RuntimeRoots{
      temporary.path() / "config", temporary.path() / "data", GISLAND_TEST_ASSET_ROOT});

  REQUIRE(bootstrap.has_value());
  CHECK(bootstrap->config_path == std::filesystem::path{GISLAND_TEST_ASSET_ROOT} / "config.toml");
  CHECK(bootstrap->config.default_module == "clock");
  REQUIRE(bootstrap->config.modules.size() == 2);
  CHECK(bootstrap->config.modules.front().module_id == "clock-calendar");
  CHECK(bootstrap->config.modules.front().command.front() == "gisland-clock-calendar");
  CHECK(bootstrap->config.modules[1].module_id == "notifications");
  CHECK(bootstrap->config.modules[1].command.front() == "gisland-notifications");
  REQUIRE(bootstrap->manifest_paths.size() == 2);
  CHECK_FALSE(bootstrap->config.modules[1].view.has_value());
  const auto *clock_view =
      bootstrap->config.modules.front().view ? &*bootstrap->config.modules.front().view : nullptr;
  REQUIRE(clock_view != nullptr);
  CHECK(clock_view->expanded.has_value());
  const auto &compact = std::get<gisland::TemplateRow>(clock_view->compact.value);
  REQUIRE(compact.children.size() == 3);
  const auto &primary = std::get<gisland::TemplateText>(
      std::get<gisland::SceneTemplatePtr>(compact.children[0])->value);
  const auto &secondary = std::get<gisland::TemplateText>(
      std::get<gisland::SceneTemplatePtr>(compact.children[2])->value);
  CHECK(std::get<std::string>(primary.role) == "compact-primary");
  CHECK(std::get<std::string>(secondary.role) == "compact-secondary");
}

TEST_CASE("bootstrap reports a missing config without creating graphical state") {
  TemporaryDirectory temporary;
  const auto result = gisland::load_runtime_bootstrap(gisland::RuntimeRoots{
      temporary.path(), temporary.path() / "data", temporary.path() / "distributed"});
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().stage == gisland::BootstrapStage::configuration);
  CHECK(result.error().path == temporary.path() / "distributed/config.toml");
}
