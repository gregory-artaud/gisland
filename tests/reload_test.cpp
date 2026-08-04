#include "gisland/reload.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
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
  auto next_disabled = disabled;
  next_disabled.enabled = false;
  auto next_enabled = enabled;
  next_enabled.enabled = true;
  auto added = module("added", "added-command");
  const auto candidate =
      config({added, next_enabled, stable, next_view, next_process, next_disabled});

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
