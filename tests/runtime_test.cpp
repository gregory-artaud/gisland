#include "gisland/runtime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <variant>

namespace {

gisland::SceneTemplate text_template(std::string path) {
  return gisland::SceneTemplate{gisland::TemplateText{
      .value = gisland::DataBinding{std::move(path)},
      .role = std::string{"body"},
  }};
}

gisland::AppConfig config() {
  gisland::ModuleInstanceConfig clock;
  clock.id = "clock";
  clock.command = {"clock-module"};
  clock.view = gisland::ModuleInstanceConfig::View{
      .compact = text_template("label"),
      .expanded = text_template("details"),
  };
  gisland::ModuleInstanceConfig status;
  status.id = "status";
  status.command = {"status-module"};
  return gisland::AppConfig{
      .monitor = "primary",
      .theme = "default",
      .default_module = "clock",
      .interaction = {},
      .modules = {std::move(clock), std::move(status)},
  };
}

gisland::ModuleMessageEvent message(std::string instance_id, gisland::ModuleMessage value,
                                    gisland::MonotonicTime at = {}) {
  return gisland::ModuleMessageEvent{std::move(instance_id), std::move(value), at};
}

const gisland::Text &text(const gisland::SceneNode &scene) {
  return std::get<gisland::Text>(scene.value);
}

} // namespace

TEST_CASE("runtime data snapshots atomically publish the configured default context") {
  gisland::RuntimeCoordinator runtime{config()};
  const auto now = gisland::MonotonicTime{} + std::chrono::seconds{1};

  REQUIRE(runtime
              .consume(message("clock",
                               gisland::DataMessage{nlohmann::json{
                                   {"label", "12:34"},
                                   {"details", "Monday"},
                               }},
                               now))
              .has_value());
  const auto selection = runtime.active(now);
  REQUIRE(selection.context != nullptr);
  CHECK(selection.context->key == gisland::ContextKey{"clock", "configured"});
  CHECK(text(selection.context->compact).value == "12:34");
  REQUIRE(selection.context->expanded.has_value());
  CHECK(text(*selection.context->expanded).value == "Monday");

  const auto revision = selection.revision;
  const auto invalid = runtime.consume(
      message("clock", gisland::DataMessage{nlohmann::json{{"label", "broken"}}}, now));
  REQUIRE_FALSE(invalid.has_value());
  const auto retained = runtime.active(now);
  REQUIRE(retained.context != nullptr);
  CHECK(retained.revision == revision);
  CHECK(text(retained.context->compact).value == "12:34");
}

TEST_CASE("runtime arbitrates direct publications, dismissals, and rejected layouts") {
  gisland::RuntimeCoordinator runtime{config()};
  const auto now = gisland::MonotonicTime{} + std::chrono::seconds{2};
  REQUIRE(
      runtime
          .consume(message(
              "clock",
              gisland::DataMessage{nlohmann::json{{"label", "12:34"}, {"details", "Monday"}}}, now))
          .has_value());
  REQUIRE(
      runtime
          .consume(message("status",
                           gisland::PublishMessage{
                               "alert", 10, std::nullopt,
                               gisland::SceneNode{gisland::Text{"Warning", "body"}}, std::nullopt},
                           now))
          .has_value());

  auto selection = runtime.active(now);
  REQUIRE(selection.context != nullptr);
  CHECK(selection.context->key == gisland::ContextKey{"status", "alert"});
  runtime.reject(selection.context->key);
  selection = runtime.active(now);
  REQUIRE(selection.context != nullptr);
  CHECK(selection.context->key == gisland::ContextKey{"clock", "configured"});

  REQUIRE(
      runtime.consume(message("clock", gisland::DismissMessage{"configured"}, now)).has_value());
  CHECK(runtime.active(now).context == nullptr);
}

TEST_CASE("runtime removes stopped instances and emits only changed ready visibility") {
  gisland::RuntimeCoordinator runtime{config()};
  const auto now = gisland::MonotonicTime{} + std::chrono::seconds{3};
  REQUIRE(runtime.consume(message("clock", gisland::ReadyMessage{1, 1, {"data-snapshots"}}, now))
              .has_value());
  REQUIRE(runtime.consume(message("status", gisland::ReadyMessage{1, 0, {}}, now)).has_value());
  REQUIRE(
      runtime
          .consume(message(
              "clock",
              gisland::DataMessage{nlohmann::json{{"label", "12:34"}, {"details", "Monday"}}}, now))
          .has_value());

  const auto compact = runtime.visibility_updates(now, gisland::IslandMode::compact);
  REQUIRE(compact.size() == 2);
  CHECK(compact[0] == gisland::VisibilityUpdate{"clock", gisland::Visibility::compact_active});
  CHECK(compact[1] == gisland::VisibilityUpdate{"status", gisland::Visibility::hidden});
  CHECK(runtime.visibility_updates(now, gisland::IslandMode::compact).empty());

  const auto expanded = runtime.visibility_updates(now, gisland::IslandMode::expanded);
  REQUIRE(expanded.size() == 1);
  CHECK(expanded[0] == gisland::VisibilityUpdate{"clock", gisland::Visibility::expanded_active});

  REQUIRE(runtime.consume(gisland::ContextsRemovedEvent{"clock", now}).has_value());
  CHECK(runtime.active(now).context == nullptr);
  CHECK(runtime.visibility_updates(now, gisland::IslandMode::compact).empty());
}

TEST_CASE("runtime start requests preserve process config and offer snapshot capability") {
  gisland::ModuleInstanceConfig module;
  module.id = "clock";
  module.command = {"python", "clock.py"};
  module.environment = {{"CLOCK_STYLE", "short"}};
  module.working_directory = "/tmp/clock";
  module.options = {
      {"format", gisland::ConfigValue{std::string{"24h"}}},
      {"week_start", gisland::ConfigValue{std::int64_t{1}}},
  };
  module.view = gisland::ModuleInstanceConfig::View{
      .compact = text_template("label"),
      .expanded = std::nullopt,
  };

  const auto request = gisland::make_module_start_request(module, "fr_FR.UTF-8", "Europe/Paris");
  CHECK(request.instance_id == "clock");
  CHECK(request.process.argv == std::vector<std::string>{"python", "clock.py"});
  CHECK(request.process.environment == module.environment);
  CHECK(request.process.working_directory == module.working_directory);
  CHECK(request.init.minimum == gisland::ProtocolVersion{1, 0});
  CHECK(request.init.maximum == gisland::ProtocolVersion{1, 1});
  CHECK(request.init.capabilities == std::vector<std::string>{"data-snapshots"});
  CHECK(request.init.configuration == nlohmann::json{{"format", "24h"}, {"week_start", 1}});
  CHECK(request.init.locale == "fr_FR.UTF-8");
  CHECK(request.init.timezone == "Europe/Paris");
}

TEST_CASE("supervised data flows through runtime into an active configured context") {
  using namespace std::chrono_literals;
  auto application = config();
  auto &module = application.modules.front();
  module.command = {GISLAND_FAKE_MODULE_PATH, "data"};
  module.view = gisland::ModuleInstanceConfig::View{
      .compact = text_template("time"),
      .expanded = text_template("time"),
  };
  module.restart = gisland::RestartPolicy::never;
  module.timings.handshake = 500ms;
  module.timings.graceful_shutdown = 50ms;
  module.timings.terminate_grace = 30ms;
  gisland::RuntimeCoordinator runtime{application};
  gisland::ModuleSupervisor supervisor;
  REQUIRE(supervisor.start(gisland::make_module_start_request(module, "C", "UTC")).has_value());

  const auto deadline = std::chrono::steady_clock::now() + 3s;
  const gisland::PublishedContext *active = nullptr;
  while (active == nullptr && std::chrono::steady_clock::now() < deadline) {
    for (const auto &event : supervisor.wait_for_events(50ms)) {
      REQUIRE(runtime.consume(event).has_value());
    }
    active = runtime.active(std::chrono::steady_clock::now()).context;
  }
  REQUIRE(active != nullptr);
  CHECK(active->key == gisland::ContextKey{"clock", "configured"});
  CHECK(text(active->compact).value == "14:35");
  supervisor.shutdown();
}
