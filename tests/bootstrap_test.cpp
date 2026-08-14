#include "gisland/bootstrap.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
  CHECK(bootstrap->theme.progress().compact_height == 48.0);
  CHECK(bootstrap->theme.views().compact.border == 0.0);
  CHECK(bootstrap->theme.views().expanded.min_height == 96.0);
  CHECK(bootstrap->theme.views().expanded.border == 0.0);
  CHECK(bootstrap->theme.typography().at("compact-primary").size == 12.0);
  CHECK(bootstrap->theme.typography().at("compact-secondary").size == 12.0);
  REQUIRE(std::holds_alternative<std::string>(bootstrap->theme.buttons().background));
  REQUIRE(std::holds_alternative<std::string>(bootstrap->theme.buttons().disabled_background));
  CHECK(std::get<std::string>(bootstrap->theme.buttons().background) == "surface");
  CHECK(std::get<std::string>(bootstrap->theme.buttons().disabled_background) == "surface");
  REQUIRE(std::holds_alternative<gisland::Rgba>(bootstrap->theme.buttons().hover_overlay));
  CHECK(std::get<gisland::Rgba>(bootstrap->theme.buttons().hover_overlay) ==
        gisland::Rgba{255, 255, 255, 20});
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

  if (!bootstrap.has_value()) {
    FAIL(bootstrap.error().path << ": " << bootstrap.error().message);
  }
  REQUIRE(bootstrap.has_value());
  CHECK(bootstrap->config_path == std::filesystem::path{GISLAND_TEST_ASSET_ROOT} / "config.toml");
  CHECK(bootstrap->config.default_module == "clock");
  REQUIRE(bootstrap->config.modules.size() == 4);
  CHECK(bootstrap->config.modules.front().module_id == "clock-calendar");
  const auto clock_package =
      std::filesystem::path{GISLAND_TEST_ASSET_ROOT} / "modules/clock-calendar";
  CHECK(bootstrap->config.modules.front().command.front() == "gisland-lua-host");
  REQUIRE(bootstrap->config.modules.front().command.size() == 3);
  CHECK(bootstrap->config.modules.front().command[1] ==
        std::filesystem::canonical(clock_package / "clock_calendar.lua"));
  CHECK(bootstrap->config.modules.front().minimum_protocol == gisland::ProtocolVersion{1, 8});
  CHECK(bootstrap->config.modules.front().maximum_protocol == gisland::ProtocolVersion{1, 9});
  REQUIRE(bootstrap->config.modules.front().view.has_value());
  CHECK(std::get<std::string>(bootstrap->config.modules.front().options.at("week_start").value) ==
        "monday");
  CHECK(bootstrap->config.modules[1].module_id == "notifications");
  const auto notification_package =
      std::filesystem::path{GISLAND_TEST_ASSET_ROOT} / "modules/notifications";
  CHECK(bootstrap->config.modules[1].command.front() == "gisland-lua-host");
  REQUIRE(bootstrap->config.modules[1].command.size() == 3);
  CHECK(bootstrap->config.modules[1].command[1] ==
        std::filesystem::canonical(notification_package / "notifications.lua"));
  CHECK(bootstrap->config.modules[1].minimum_protocol == gisland::ProtocolVersion{1, 8});
  CHECK(bootstrap->config.modules[1].maximum_protocol == gisland::ProtocolVersion{1, 8});
  CHECK(std::get<std::int64_t>(
            bootstrap->config.modules[1].options.at("reveal_duration_ms").value) == 1000);
  CHECK(std::get<std::int64_t>(bootstrap->config.modules[1].options.at("history_limit").value) ==
        100);
  CHECK(std::get<std::int64_t>(
            bootstrap->config.modules[1].options.at("history_visible_limit").value) == 5);
  CHECK(bootstrap->config.modules[2].module_id == "battery");
  const auto battery_package = std::filesystem::path{GISLAND_TEST_ASSET_ROOT} / "modules/battery";
  CHECK(bootstrap->config.modules[2].command.front() == "gisland-lua-host");
  REQUIRE(bootstrap->config.modules[2].command.size() == 3);
  CHECK(bootstrap->config.modules[2].command[1] ==
        std::filesystem::canonical(battery_package / "battery.lua"));
  CHECK(bootstrap->config.modules[2].minimum_protocol == gisland::ProtocolVersion{1, 8});
  CHECK(bootstrap->config.modules[2].maximum_protocol == gisland::ProtocolVersion{1, 8});
  CHECK(std::get<std::int64_t>(bootstrap->config.modules[2].options.at("warning_percent").value) ==
        20);
  CHECK(std::get<std::int64_t>(
            bootstrap->config.modules[2].options.at("persistent_percent").value) == 10);
  REQUIRE(bootstrap->config.modules[2].dependencies.config.has_value());
  CHECK(bootstrap->config.modules[2].dependencies.config->path ==
        std::filesystem::canonical(battery_package / "config.toml"));
  REQUIRE(bootstrap->config.modules[2].dependencies.entry.has_value());
  CHECK(bootstrap->config.modules[2].dependencies.entry->path ==
        std::filesystem::canonical(battery_package / "battery.lua"));
  CHECK(bootstrap->config.modules[3].module_id == "audio");
  const auto audio_package = std::filesystem::path{GISLAND_TEST_ASSET_ROOT} / "modules/audio";
  CHECK(bootstrap->config.modules[3].command.front() == "gisland-lua-host");
  REQUIRE(bootstrap->config.modules[3].command.size() == 3);
  CHECK(bootstrap->config.modules[3].command[1] ==
        std::filesystem::canonical(audio_package / "audio.lua"));
  CHECK(bootstrap->config.modules[3].minimum_protocol == gisland::ProtocolVersion{1, 8});
  CHECK(bootstrap->config.modules[3].maximum_protocol == gisland::ProtocolVersion{1, 8});
  CHECK(std::get<std::int64_t>(bootstrap->config.modules[3].options.at("step_percent").value) == 5);
  CHECK(std::get<std::int64_t>(bootstrap->config.modules[3].options.at("maximum_percent").value) ==
        150);
  CHECK(std::get<std::int64_t>(bootstrap->config.modules[3].options.at("hud_duration_ms").value) ==
        1500);
  REQUIRE(bootstrap->config.modules[3].dependencies.config.has_value());
  CHECK(bootstrap->config.modules[3].dependencies.config->path ==
        std::filesystem::canonical(audio_package / "config.toml"));
  REQUIRE(bootstrap->config.modules[3].dependencies.entry.has_value());
  CHECK(bootstrap->config.modules[3].dependencies.entry->path ==
        std::filesystem::canonical(audio_package / "audio.lua"));
  CHECK(std::filesystem::is_regular_file(audio_package / "command.lua"));
  REQUIRE(bootstrap->manifest_paths.size() == 4);
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
  const auto &expanded = std::get<gisland::TemplateColumn>(clock_view->expanded->value);
  REQUIRE(expanded.children.size() == 2);
  const auto &header = std::get<gisland::TemplateRow>(
      std::get<gisland::SceneTemplatePtr>(expanded.children.front())->value);
  REQUIRE(header.children.size() == 5);
  const auto &previous = std::get<gisland::TemplateButton>(
      std::get<gisland::SceneTemplatePtr>(header.children.front())->value);
  const auto &heading = std::get<gisland::TemplateColumn>(
      std::get<gisland::SceneTemplatePtr>(header.children[2])->value);
  const auto &today = std::get<gisland::TemplateButton>(
      std::get<gisland::SceneTemplatePtr>(heading.children[1])->value);
  const auto &next = std::get<gisland::TemplateButton>(
      std::get<gisland::SceneTemplatePtr>(header.children.back())->value);
  CHECK(previous.action_id == "previous-month");
  CHECK(today.action_id == "today");
  CHECK(next.action_id == "next-month");
  REQUIRE(bootstrap->config.modules[2].view.has_value());
  const auto &battery_compact =
      std::get<gisland::TemplateRow>(bootstrap->config.modules[2].view->compact.value);
  REQUIRE(battery_compact.children.size() == 4);
  const auto &battery_progress = std::get<gisland::TemplateProgress>(
      std::get<gisland::SceneTemplatePtr>(battery_compact.children.front())->value);
  CHECK(std::get<std::string>(battery_progress.shape) == "ring");
}

TEST_CASE("bootstrap reports a missing config without creating graphical state") {
  TemporaryDirectory temporary;
  const auto result = gisland::load_runtime_bootstrap(gisland::RuntimeRoots{
      temporary.path(), temporary.path() / "data", temporary.path() / "distributed"});
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().stage == gisland::BootstrapStage::configuration);
  CHECK(result.error().path == temporary.path() / "distributed/config.toml");
}

TEST_CASE("bootstrap resolves and tracks a config-root module manifest") {
  TemporaryDirectory temporary;
  const auto config_home = temporary.path() / "config";
  const auto manifest_path = config_home / "gisland/modules/personal/module.toml";
  write_file(config_home / "gisland/config.toml", "monitor = \"primary\"\n"
                                                  "theme = \"default\"\n"
                                                  "default_module = \"personal\"\n"
                                                  "[[modules]]\n"
                                                  "id = \"personal\"\n"
                                                  "module = \"personal\"\n");
  write_file(manifest_path, "id = \"personal\"\n"
                            "name = \"Personal\"\n"
                            "command = [\"./personal.py\"]\n"
                            "[protocol]\n"
                            "major = 1\n"
                            "minimum_minor = 5\n"
                            "maximum_minor = 5\n");

  const auto bootstrap = gisland::load_runtime_bootstrap(
      {config_home, temporary.path() / "data", GISLAND_TEST_ASSET_ROOT});

  REQUIRE(bootstrap.has_value());
  REQUIRE(bootstrap->config.modules.size() == 1);
  CHECK(bootstrap->config.modules.front().manifest_path == manifest_path);
  CHECK(bootstrap->manifest_paths == std::vector<std::filesystem::path>{manifest_path});
}

TEST_CASE("bootstrap resolves package defaults and independently overridden view slots") {
  TemporaryDirectory temporary;
  const auto config_home = temporary.path() / "config";
  const auto package = config_home / "gisland/modules/packaged";
  write_file(config_home / "gisland/config.toml", R"(
monitor = "primary"
theme = "default"
default_module = "packaged"
[[modules]]
id = "packaged"
module = "packaged"
[modules.options]
level = 2
[modules.view.expanded]
type = "text"
value = "instance expanded"
role = "body"
)");
  write_file(package / "config.toml", "[defaults]\nmode=\"quiet\"\nlevel=1\n");
  write_file(package / "view.toml", R"(
[compact]
type = "text"
value = { bind = "label" }
role = "body"
[expanded]
type = "text"
value = "package expanded"
role = "body"
)");
  write_file(package / "module.toml", R"(
id = "packaged"
name = "Packaged"
command = ["/bin/true"]
config = "config.toml"
view = "view.toml"
[protocol]
major = 1
minimum_minor = 0
maximum_minor = 8
[options_schema.mode]
type = "string"
[options_schema.level]
type = "integer"
)");

  const auto bootstrap = gisland::load_runtime_bootstrap(
      {config_home, temporary.path() / "data", GISLAND_TEST_ASSET_ROOT});

  REQUIRE(bootstrap.has_value());
  REQUIRE(bootstrap->config.modules.size() == 1);
  const auto &module = bootstrap->config.modules.front();
  CHECK(std::get<std::string>(module.options.at("mode").value) == "quiet");
  CHECK(std::get<std::int64_t>(module.options.at("level").value) == 2);
  REQUIRE(module.view.has_value());
  const auto &compact = std::get<gisland::TemplateText>(module.view->compact.value);
  CHECK(std::holds_alternative<gisland::DataBinding>(compact.value));
  REQUIRE(module.view->expanded.has_value());
  const auto &expanded = std::get<gisland::TemplateText>(module.view->expanded->value);
  CHECK(std::get<std::string>(expanded.value) == "instance expanded");
}

TEST_CASE("bootstrap materializes one canonical absolute Lua entry before user arguments") {
  TemporaryDirectory temporary;
  const auto config_home = temporary.path() / "config";
  const auto package = config_home / "gisland/modules/lua-package";
  write_file(config_home / "gisland/config.toml", R"(
monitor = "primary"
theme = "default"
default_module = "lua"
[[modules]]
id = "lua"
module = "lua-package"
arguments = ["--user-option", "other.lua"]
working_directory = "/tmp"
)");
  write_file(package / "entry.lua", "return gisland.module {}\n");
  write_file(package / "module.toml", R"(
id = "lua-package"
name = "Lua package"
command = ["gisland-lua-host", "--manifest-option"]
entry = "entry.lua"
[protocol]
major = 1
minimum_minor = 8
maximum_minor = 8
)");

  const auto bootstrap = gisland::load_runtime_bootstrap(
      {config_home, temporary.path() / "data", GISLAND_TEST_ASSET_ROOT});

  REQUIRE(bootstrap.has_value());
  REQUIRE(bootstrap->config.modules.size() == 1);
  const auto entry = std::filesystem::canonical(package / "entry.lua").string();
  const auto &entry_fingerprint = bootstrap->config.modules.front().dependencies.entry->fingerprint;
  CHECK(bootstrap->config.modules.front().command ==
        std::vector<std::string>{"gisland-lua-host", entry,
                                 "--gisland-entry-fingerprint=" + entry_fingerprint,
                                 "--manifest-option", "--user-option", "other.lua"});
  CHECK(std::ranges::count(bootstrap->config.modules.front().command, entry) == 1);
}

TEST_CASE(
    "bootstrap enables the distributed Lua example without duplicating package defaults or view") {
  TemporaryDirectory temporary;
  const auto config_home = temporary.path() / "config";
  write_file(config_home / "gisland/config.toml", R"(
monitor = "primary"
theme = "default"
default_module = "example"
[[modules]]
id = "example"
module = "lua-example"
)");

  const auto bootstrap = gisland::load_runtime_bootstrap(
      {config_home, temporary.path() / "data", GISLAND_TEST_ASSET_ROOT});

  REQUIRE(bootstrap.has_value());
  REQUIRE(bootstrap->config.modules.size() == 1);
  const auto &module = bootstrap->config.modules.front();
  CHECK(module.command.front() == "gisland-lua-host");
  REQUIRE(module.command.size() == 3);
  CHECK(module.command[1] ==
        std::filesystem::canonical(std::filesystem::path{GISLAND_TEST_ASSET_ROOT} /
                                   "modules/lua-example/example.lua"));
  CHECK(module.command[2] ==
        "--gisland-entry-fingerprint=" + module.dependencies.entry->fingerprint);
  CHECK(std::get<std::string>(module.options.at("format").value) == "%H:%M:%S");
  REQUIRE(module.view.has_value());
  const auto &compact = std::get<gisland::TemplateText>(module.view->compact.value);
  REQUIRE(std::holds_alternative<gisland::DataBinding>(compact.value));
  CHECK(std::get<gisland::DataBinding>(compact.value).path == "time");
}

TEST_CASE("bootstrap watches enabled package static files but not required Lua dependencies") {
  TemporaryDirectory temporary;
  const auto config_home = temporary.path() / "config";
  const auto package = config_home / "gisland/modules/lua-package";
  write_file(config_home / "gisland/config.toml", R"(
monitor = "primary"
theme = "default"
default_module = "enabled"
[[modules]]
id = "enabled"
module = "lua-package"
[[modules]]
id = "disabled"
module = "lua-package"
enabled = false
)");
  write_file(package / "entry.lua", "require('dependency')\nreturn gisland.module {}\n");
  write_file(package / "dependency.lua", "return {}\n");
  write_file(package / "config.toml", "[defaults]\nmode = \"quiet\"\n");
  write_file(package / "view.toml",
             "[compact]\ntype = \"text\"\nvalue = \"ready\"\nrole = \"body\"\n");
  write_file(package / "module.toml", R"(
id = "lua-package"
name = "Lua package"
command = ["gisland-lua-host"]
entry = "entry.lua"
config = "config.toml"
view = "view.toml"
[protocol]
major = 1
minimum_minor = 8
maximum_minor = 8
[options_schema.mode]
type = "string"
)");

  const auto bootstrap = gisland::load_runtime_bootstrap(
      {config_home, temporary.path() / "data", GISLAND_TEST_ASSET_ROOT});

  REQUIRE(bootstrap.has_value());
  CHECK(bootstrap->module_dependency_paths ==
        std::vector<std::filesystem::path>{std::filesystem::canonical(package / "module.toml"),
                                           std::filesystem::canonical(package / "config.toml"),
                                           std::filesystem::canonical(package / "view.toml"),
                                           std::filesystem::canonical(package / "entry.lua")});
  CHECK(std::ranges::find(bootstrap->module_dependency_paths, package / "dependency.lua") ==
        bootstrap->module_dependency_paths.end());
}
