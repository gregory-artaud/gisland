#include "gisland/runtime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

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
  gisland::ModuleInstanceConfig disabled;
  disabled.id = "disabled";
  disabled.command = {"disabled-module"};
  disabled.enabled = false;
  return gisland::AppConfig{
      .monitor = "primary",
      .theme = "default",
      .default_module = "clock",
      .interaction = {},
      .modules = {std::move(clock), std::move(status), std::move(disabled)},
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

TEST_CASE("runtime rejection restores the last accepted context resources atomically") {
  gisland::RuntimeCoordinator runtime{config()};
  const auto now = gisland::MonotonicTime{} + std::chrono::seconds{2};
  const auto key = gisland::ContextKey{"status", "alert"};
  const auto old_pixels =
      std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{255, 0, 0, 255});
  REQUIRE(
      runtime
          .consume(message(
              "status",
              gisland::PublishMessage{
                  "alert",
                  10,
                  std::nullopt,
                  gisland::SceneNode{gisland::Image{"icon", "notification-icon", "Application"}},
                  std::nullopt,
                  {gisland::ImageResource{"icon", gisland::ImageFormat::rgba8, 1, 1, old_pixels}}},
              now))
          .has_value());
  runtime.accept(key);

  REQUIRE(runtime
              .consume(message(
                  "status",
                  gisland::PublishMessage{
                      "alert",
                      20,
                      std::nullopt,
                      gisland::SceneNode{gisland::Image{"icon", "missing-role", "Application"}},
                      std::nullopt,
                      {gisland::ImageResource{"icon", gisland::ImageFormat::rgba8, 1, 1,
                                              std::make_shared<const std::vector<std::uint8_t>>(
                                                  std::vector<std::uint8_t>{0, 0, 255, 255})}}},
                  now + std::chrono::milliseconds{1}))
              .has_value());

  runtime.reject(key, now + std::chrono::milliseconds{1});

  const auto retained = runtime.active(now + std::chrono::milliseconds{1});
  REQUIRE(retained.context != nullptr);
  CHECK(retained.context->priority == 10);
  REQUIRE(retained.context->resources.size() == 1);
  CHECK(retained.context->resources.front().pixels == old_pixels);
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
  CHECK(request.init.maximum == gisland::ProtocolVersion{1, 3});
  CHECK(request.init.capabilities ==
        std::vector<std::string>{"data-snapshots", "context-images", "rich-content"});
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

TEST_CASE("runtime activation overrides global arbitration until its deadline") {
  using namespace std::chrono_literals;
  gisland::RuntimeCoordinator runtime{config()};
  const auto now = gisland::MonotonicTime{} + 1s;
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
                               "alert", 100, std::nullopt,
                               gisland::SceneNode{gisland::Text{"Warning", "body"}}, std::nullopt},
                           now + 1ms))
          .has_value());
  REQUIRE(runtime.active(now + 1ms).context != nullptr);
  CHECK(runtime.active(now + 1ms).context->key == gisland::ContextKey{"status", "alert"});

  const auto activated = runtime.activate("clock", 10ms, now + 1ms);
  REQUIRE(activated.has_value());
  CHECK((*activated == gisland::ContextKey{"clock", "configured"}));
  REQUIRE(runtime.active(now + 10ms).context != nullptr);
  CHECK(runtime.active(now + 10ms).context->key == gisland::ContextKey{"clock", "configured"});
  REQUIRE(runtime.active(now + 11ms).context != nullptr);
  CHECK(runtime.active(now + 11ms).context->key == gisland::ContextKey{"status", "alert"});
}

TEST_CASE("runtime control rejects unknown disabled unavailable and mismatched contexts") {
  gisland::RuntimeCoordinator runtime{config()};
  const gisland::MonotonicTime now{};

  CHECK(runtime.activate("missing", std::nullopt, now).error().code ==
        gisland::RuntimeErrorCode::unknown_instance);
  CHECK(runtime.activate("disabled", std::nullopt, now).error().code ==
        gisland::RuntimeErrorCode::disabled_instance);
  CHECK(runtime.activate("status", std::nullopt, now).error().code ==
        gisland::RuntimeErrorCode::unavailable_instance);
  CHECK(runtime.dismiss_active("missing", now).error().code ==
        gisland::RuntimeErrorCode::unknown_context);
}

TEST_CASE("runtime dismisses only the matching active context") {
  gisland::RuntimeCoordinator runtime{config()};
  const gisland::MonotonicTime now{};
  REQUIRE(
      runtime
          .consume(message(
              "clock",
              gisland::DataMessage{nlohmann::json{{"label", "12:34"}, {"details", "Monday"}}}, now))
          .has_value());

  CHECK_FALSE(runtime.dismiss_active("other", now).has_value());
  const auto dismissed = runtime.dismiss_active("configured", now);
  REQUIRE(dismissed.has_value());
  CHECK((*dismissed == gisland::ContextKey{"clock", "configured"}));
  CHECK(runtime.active(now).context == nullptr);
}

TEST_CASE("runtime module snapshots retain configured order state and availability") {
  gisland::RuntimeCoordinator runtime{config()};
  const gisland::MonotonicTime now{};
  REQUIRE(
      runtime
          .consume(message(
              "clock",
              gisland::DataMessage{nlohmann::json{{"label", "12:34"}, {"details", "Monday"}}}, now))
          .has_value());
  REQUIRE(runtime
              .consume(gisland::StateChangedEvent{
                  "clock", gisland::StateTransition{gisland::ModuleState::starting,
                                                    gisland::ModuleState::running,
                                                    gisland::StopCause::requested, now}})
              .has_value());

  const auto modules = runtime.module_statuses(now);
  REQUIRE(modules.size() == 3);
  CHECK(modules[0] ==
        gisland::RuntimeModuleStatus{"clock", true, gisland::ModuleState::running, true});
  CHECK(modules[1] ==
        gisland::RuntimeModuleStatus{"status", true, gisland::ModuleState::stopped, false});
  CHECK(modules[2] ==
        gisland::RuntimeModuleStatus{"disabled", false, gisland::ModuleState::stopped, false});
}

TEST_CASE(
    "runtime reload preserves unchanged state and removes affected state in candidate order") {
  using namespace std::chrono_literals;
  auto current = config();
  gisland::RuntimeCoordinator runtime{current};
  const auto now = gisland::MonotonicTime{} + 5s;
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
  REQUIRE(runtime
              .consume(gisland::StateChangedEvent{
                  "clock", gisland::StateTransition{gisland::ModuleState::starting,
                                                    gisland::ModuleState::running,
                                                    gisland::StopCause::requested, now}})
              .has_value());
  REQUIRE(runtime.activate("clock", 10s, now).has_value());

  auto candidate = current;
  candidate.default_module = "disabled";
  candidate.modules[1].command = {"new-status-module"};
  candidate.modules[2].enabled = true;
  candidate.modules = {candidate.modules[2], candidate.modules[0], candidate.modules[1]};
  const auto plan = gisland::plan_reload(current, candidate, "C", "UTC");
  REQUIRE(plan.has_value());
  auto prepared = runtime.prepare_reload(*plan);
  REQUIRE(prepared.has_value());
  runtime.commit_reload(std::move(*prepared));

  const auto modules = runtime.module_statuses(now + 1s);
  REQUIRE(modules.size() == 3);
  CHECK(modules[0] ==
        gisland::RuntimeModuleStatus{"disabled", true, gisland::ModuleState::stopped, false});
  CHECK(modules[1] ==
        gisland::RuntimeModuleStatus{"clock", true, gisland::ModuleState::running, true});
  CHECK(modules[2] ==
        gisland::RuntimeModuleStatus{"status", true, gisland::ModuleState::stopped, false});
  REQUIRE(runtime.active(now + 1s).context != nullptr);
  CHECK(runtime.active(now + 1s).context->key == gisland::ContextKey{"clock", "configured"});

  REQUIRE(runtime.consume(gisland::ContextsRemovedEvent{"status", now + 2s}).has_value());
  CHECK(runtime.module_statuses(now + 2s)[2].available == false);
}

TEST_CASE("runtime preflights view reloads from retained snapshots without mutating on rejection") {
  auto current = config();
  gisland::RuntimeCoordinator runtime{current};
  const auto now = gisland::MonotonicTime{} + std::chrono::seconds{6};
  REQUIRE(
      runtime
          .consume(message(
              "clock",
              gisland::DataMessage{nlohmann::json{{"label", "12:34"}, {"details", "Monday"}}}, now))
          .has_value());

  auto invalid_candidate = current;
  invalid_candidate.modules[0].view = gisland::ModuleInstanceConfig::View{
      .compact = text_template("missing"), .expanded = text_template("details")};
  const auto invalid_plan = gisland::plan_reload(current, invalid_candidate, "C", "UTC");
  REQUIRE(invalid_plan.has_value());
  CHECK_FALSE(runtime.prepare_reload(*invalid_plan).has_value());
  REQUIRE(runtime.active(now).context != nullptr);
  CHECK(text(runtime.active(now).context->compact).value == "12:34");

  auto candidate = current;
  candidate.modules[0].view = gisland::ModuleInstanceConfig::View{
      .compact = text_template("details"), .expanded = text_template("details")};
  const auto plan = gisland::plan_reload(current, candidate, "C", "UTC");
  REQUIRE(plan.has_value());
  auto prepared = runtime.prepare_reload(*plan);
  REQUIRE(prepared.has_value());
  runtime.commit_reload(std::move(*prepared));
  REQUIRE(runtime.active(now).context != nullptr);
  CHECK(text(runtime.active(now).context->compact).value == "Monday");
}

TEST_CASE("runtime view removal discards only the configured snapshot context") {
  auto current = config();
  gisland::RuntimeCoordinator runtime{current};
  const gisland::MonotonicTime now{};
  REQUIRE(
      runtime
          .consume(message(
              "clock",
              gisland::DataMessage{nlohmann::json{{"label", "12:34"}, {"details", "Monday"}}}, now))
          .has_value());

  auto candidate = current;
  candidate.modules[0].view.reset();
  const auto plan = gisland::plan_reload(current, candidate, "C", "UTC");
  REQUIRE(plan.has_value());
  auto prepared = runtime.prepare_reload(*plan);
  REQUIRE(prepared.has_value());
  runtime.commit_reload(std::move(*prepared));
  CHECK(runtime.active(now).context == nullptr);
}
