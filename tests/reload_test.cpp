#include "gisland/reload.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

gisland::SceneTemplate text_template(std::string value) {
  return gisland::SceneTemplate{gisland::TemplateText{
      .value = std::move(value),
      .role = std::string{"body"},
  }};
}

gisland::ModuleInstanceConfig module(std::string id, std::string command) {
  gisland::ModuleInstanceConfig result;
  result.id = std::move(id);
  result.command = {std::move(command)};
  result.view = gisland::ModuleInstanceConfig::View{.compact = text_template("old"),
                                                    .expanded = std::nullopt};
  return result;
}

gisland::AppConfig config(std::vector<gisland::ModuleInstanceConfig> modules) {
  return gisland::AppConfig{.monitor = "primary",
                            .theme = "default",
                            .default_module = "stable",
                            .interaction = {},
                            .modules = std::move(modules)};
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() / ("gisland-reload-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path &path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream{path};
  if (!stream) {
    throw std::runtime_error{"could not write reload fixture"};
  }
  stream << content;
}

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream stream{path};
  if (!stream) {
    throw std::runtime_error{"could not read reload fixture"};
  }
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("reload planner classifies process and view changes in candidate order") {
  auto stable = module("stable", "stable-command");
  auto view = module("view", "view-command");
  auto process = module("process", "old-command");
  auto removed = module("removed", "removed-command");
  auto disabled = module("disabled", "disabled-command");
  auto enabled = module("enabled", "enabled-command");
  enabled.enabled = false;
  const auto current = config({stable, view, process, removed, disabled, enabled});

  auto next_view = view;
  next_view.view = gisland::ModuleInstanceConfig::View{.compact = text_template("new"),
                                                       .expanded = std::nullopt};
  auto next_process = process;
  next_process.command = {"new-command"};
  auto next_stable = stable;
  auto next_disabled = disabled;
  next_disabled.enabled = false;
  auto next_enabled = enabled;
  next_enabled.enabled = true;
  auto added = module("added", "added-command");
  auto candidate =
      config({added, next_enabled, next_stable, next_view, next_process, next_disabled});
  candidate.compact_default = "added";
  candidate.expanded_default = "view";

  const auto plan = gisland::plan_reload(current, candidate, "C", "UTC");
  REQUIRE(plan.has_value());
  CHECK(plan->changes == std::vector<gisland::ModuleReloadChange>{
                             {"added", gisland::ModuleReloadKind::added, true},
                             {"enabled", gisland::ModuleReloadKind::enabled, true},
                             {"stable", gisland::ModuleReloadKind::unchanged, false},
                             {"view", gisland::ModuleReloadKind::view_updated, false},
                             {"process", gisland::ModuleReloadKind::process_modified, true},
                             {"disabled", gisland::ModuleReloadKind::disabled, false},
                             {"removed", gisland::ModuleReloadKind::removed, false},
                         });
  CHECK(plan->start_requests.size() == 3);
  CHECK(plan->start_requests[0].instance_id == "added");
  CHECK(plan->start_requests[1].instance_id == "enabled");
  CHECK(plan->start_requests[2].instance_id == "process");
  CHECK(plan->supervisor.stop_instances == std::vector<std::string>{"disabled", "removed"});
  REQUIRE(plan->supervisor.start_or_replace.size() == 3);
  CHECK(plan->supervisor.start_or_replace[0].instance_id == "added");
  CHECK(plan->supervisor.start_or_replace[1].instance_id == "enabled");
  CHECK(plan->supervisor.start_or_replace[2].instance_id == "process");
  CHECK(plan->candidate.compact_default == "added");
  CHECK(plan->candidate.expanded_default == "view");
}

TEST_CASE("reload candidate reuses the startup config path and resolves a changed user theme") {
  TemporaryDirectory temporary;
  const auto distributed = std::filesystem::path{GISLAND_TEST_ASSET_ROOT};
  const auto config_home = temporary.path() / "config";
  const auto config_path = config_home / "gisland/config.toml";
  write_file(config_path, "monitor = \"primary\"\n"
                          "theme = \"default\"\n"
                          "default_module = \"clock\"\n"
                          "[[modules]]\n"
                          "id = \"clock\"\n"
                          "command = [\"/bin/true\"]\n");
  auto current =
      gisland::load_runtime_bootstrap({config_home, temporary.path() / "data", distributed});
  REQUIRE(current.has_value());

  std::ifstream default_theme{distributed / "themes/default.toml"};
  const std::string theme{std::istreambuf_iterator<char>{default_theme},
                          std::istreambuf_iterator<char>{}};
  write_file(config_home / "gisland/themes/custom.toml", theme);
  write_file(config_path, "monitor = \"primary\"\n"
                          "theme = \"custom\"\n"
                          "default_module = \"clock\"\n"
                          "[[modules]]\n"
                          "id = \"clock\"\n"
                          "command = [\"/bin/true\"]\n");

  const auto candidate = gisland::load_reload_candidate(*current);
  REQUIRE(candidate.has_value());
  CHECK(candidate->config.theme == "custom");
  CHECK(candidate->config_path == config_path);
  CHECK(candidate->theme_path == config_home / "gisland/themes/custom.toml");
  CHECK(candidate->asset_root == config_home / "gisland");
}

TEST_CASE("reload candidate accepts complete axis padding and rejects an incomplete pair") {
  TemporaryDirectory temporary;
  const auto distributed = std::filesystem::path{GISLAND_TEST_ASSET_ROOT};
  const auto config_home = temporary.path() / "config";
  write_file(config_home / "gisland/config.toml", "monitor = \"primary\"\n"
                                                  "theme = \"default\"\n"
                                                  "default_module = \"clock\"\n"
                                                  "[[modules]]\n"
                                                  "id = \"clock\"\n"
                                                  "command = [\"/bin/true\"]\n");
  const auto current =
      gisland::load_runtime_bootstrap({config_home, temporary.path() / "data", distributed});
  REQUIRE(current.has_value());

  std::string theme = read_file(distributed / "themes/default.toml");
  const auto horizontal = theme.find("padding_horizontal = 14");
  REQUIRE(horizontal != std::string::npos);
  theme.replace(horizontal, std::string_view{"padding_horizontal = 14"}.size(),
                "padding_horizontal = 12");
  const auto user_theme = config_home / "gisland/themes/default.toml";
  write_file(user_theme, theme);

  const auto candidate = gisland::load_reload_candidate(*current);
  REQUIRE(candidate.has_value());
  CHECK(candidate->theme.views().compact.padding_horizontal == 12.0);
  CHECK(candidate->theme.views().compact.padding_vertical == 4.0);

  const auto vertical = theme.find("padding_vertical = 4\n");
  REQUIRE(vertical != std::string::npos);
  theme.erase(vertical, std::string_view{"padding_vertical = 4\n"}.size());
  write_file(user_theme, theme);

  const auto rejected = gisland::load_reload_candidate(*current);
  REQUIRE_FALSE(rejected.has_value());
  CHECK(rejected.error().stage == gisland::BootstrapStage::theme);
  CHECK(rejected.error().message.contains("view.compact.padding_vertical"));
}

TEST_CASE("reload candidate rediscovers config-root module manifests") {
  TemporaryDirectory temporary;
  const auto distributed = std::filesystem::path{GISLAND_TEST_ASSET_ROOT};
  const auto config_home = temporary.path() / "config";
  const auto manifest_path = config_home / "gisland/modules/personal/module.toml";
  write_file(config_home / "gisland/config.toml", "monitor = \"primary\"\n"
                                                  "theme = \"default\"\n"
                                                  "default_module = \"personal\"\n"
                                                  "[[modules]]\n"
                                                  "id = \"personal\"\n"
                                                  "module = \"personal\"\n");
  const auto write_manifest = [&manifest_path](std::string_view command) {
    write_file(manifest_path,
               "id = \"personal\"\nname = \"Personal\"\ncommand = [\"" + std::string{command} +
                   "\"]\n[protocol]\nmajor = 1\nminimum_minor = 5\nmaximum_minor = 5\n");
  };
  write_manifest("./first.py");
  const auto current =
      gisland::load_runtime_bootstrap({config_home, temporary.path() / "data", distributed});
  REQUIRE(current.has_value());

  write_manifest("./second.py");
  const auto candidate = gisland::load_reload_candidate(*current);

  REQUIRE(candidate.has_value());
  CHECK(candidate->config.modules.front().command.front() ==
        (manifest_path.parent_path() / "second.py").string());
  CHECK(candidate->manifest_paths == std::vector<std::filesystem::path>{manifest_path});
}

TEST_CASE("package static dependency changes distinguish process restart from view update") {
  TemporaryDirectory temporary;
  const auto distributed = std::filesystem::path{GISLAND_TEST_ASSET_ROOT};
  const auto config_home = temporary.path() / "config";
  const auto package = config_home / "gisland/modules/packaged";
  write_file(config_home / "gisland/config.toml", R"(
monitor = "primary"
theme = "default"
default_module = "packaged"
[[modules]]
id = "packaged"
module = "packaged"
)");
  write_file(package / "entry.lua", "return gisland.module {}\n");
  write_file(package / "config.toml", "[defaults]\nmode = \"one\"\n");
  write_file(package / "view.toml",
             "[compact]\ntype = \"text\"\nvalue = \"one\"\nrole = \"body\"\n");
  const auto manifest = package / "module.toml";
  const auto manifest_text = R"(id = "packaged"
name = "Packaged"
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
)";
  write_file(manifest, manifest_text);
  const auto current =
      gisland::load_runtime_bootstrap({config_home, temporary.path() / "data", distributed});
  REQUIRE(current.has_value());

  const auto classify = [&](const gisland::RuntimeBootstrap &before) {
    const auto candidate = gisland::load_reload_candidate(before);
    REQUIRE(candidate.has_value());
    const auto plan = gisland::plan_reload(before.config, candidate->config, "C", "UTC");
    REQUIRE(plan.has_value());
    REQUIRE(plan->changes.size() == 1);
    return std::pair{std::move(*candidate), plan->changes.front().kind};
  };

  write_file(package / "view.toml",
             "[compact]\ntype = \"text\"\nvalue = \"two\"\nrole = \"body\"\n");
  auto [view_candidate, view_kind] = classify(*current);
  CHECK(view_kind == gisland::ModuleReloadKind::view_updated);
  const auto view_plan = gisland::plan_reload(current->config, view_candidate.config, "C", "UTC");
  REQUIRE(view_plan.has_value());
  CHECK(view_plan->supervisor.start_or_replace.empty());

  write_file(package / "entry.lua", "return gisland.module { init = function() end }\n");
  auto [entry_candidate, entry_kind] = classify(view_candidate);
  CHECK(entry_kind == gisland::ModuleReloadKind::process_modified);

  write_file(package / "config.toml", "[defaults]\nmode = \"two\"\n");
  auto [config_candidate, config_kind] = classify(entry_candidate);
  CHECK(config_kind == gisland::ModuleReloadKind::process_modified);

  std::string changed_manifest{manifest_text};
  const auto name = changed_manifest.find("name = \"Packaged\"");
  REQUIRE(name != std::string::npos);
  changed_manifest.insert(name, "description = \"changed\"\n");
  write_file(manifest, changed_manifest);
  auto [manifest_candidate, manifest_kind] = classify(config_candidate);
  CHECK(manifest_kind == gisland::ModuleReloadKind::process_modified);
}

TEST_CASE("invalid package static replacement rejects candidate before reconfiguration") {
  TemporaryDirectory temporary;
  const auto distributed = std::filesystem::path{GISLAND_TEST_ASSET_ROOT};
  const auto config_home = temporary.path() / "config";
  const auto package = config_home / "gisland/modules/packaged";
  write_file(config_home / "gisland/config.toml", R"(
monitor = "primary"
theme = "default"
default_module = "packaged"
[[modules]]
id = "packaged"
module = "packaged"
)");
  write_file(package / "view.toml",
             "[compact]\ntype = \"text\"\nvalue = \"valid\"\nrole = \"body\"\n");
  write_file(package / "module.toml", R"(
id = "packaged"
name = "Packaged"
command = ["/bin/true"]
view = "view.toml"
[protocol]
major = 1
minimum_minor = 0
maximum_minor = 8
)");
  const auto current =
      gisland::load_runtime_bootstrap({config_home, temporary.path() / "data", distributed});
  REQUIRE(current.has_value());

  write_file(package / "view.toml", "[compact]\ntype = \"unknown\"\n");
  const auto rejected = gisland::load_reload_candidate(*current);

  REQUIRE_FALSE(rejected.has_value());
  CHECK(rejected.error().stage == gisland::BootstrapStage::configuration);
  REQUIRE(current->config.modules.front().view.has_value());
  CHECK(std::get<std::string>(
            std::get<gisland::TemplateText>(current->config.modules.front().view->compact.value)
                .value) == "valid");
}
