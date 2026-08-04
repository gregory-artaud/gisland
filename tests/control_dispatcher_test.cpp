#include "gisland/control_dispatcher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

namespace {

gisland::AppConfig config() {
  gisland::ModuleInstanceConfig clock;
  clock.id = "clock";
  clock.command = {"clock-module"};
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

gisland::SceneNode text(std::string value) {
  return gisland::SceneNode{gisland::Text{std::move(value), "body"}};
}

void publish(gisland::RuntimeCoordinator &runtime, std::string instance_id, std::string context_id,
             int priority, bool expanded, gisland::MonotonicTime now) {
  REQUIRE(runtime
              .consume(gisland::ModuleMessageEvent{
                  std::move(instance_id),
                  gisland::PublishMessage{
                      std::move(context_id), priority, std::nullopt, text("compact"),
                      expanded ? std::optional{text("expanded")} : std::nullopt},
                  now})
              .has_value());
}

const gisland::ControlError &error(const gisland::ControlResponse &response) {
  return std::get<gisland::ControlError>(response.value());
}

} // namespace

TEST_CASE("control dispatcher mutates mode and recomputes status snapshots") {
  const gisland::MonotonicTime now{};
  gisland::RuntimeCoordinator runtime{config()};
  gisland::OverlayModeController mode;
  publish(runtime, "clock", "configured", 0, true, now);
  REQUIRE(runtime
              .consume(gisland::StateChangedEvent{"clock",
                                                  {gisland::ModuleState::starting,
                                                   gisland::ModuleState::running,
                                                   gisland::StopCause::requested, now}})
              .has_value());
  gisland::ControlDispatcher dispatcher{
      runtime, mode,
      [](std::string, std::uint64_t) -> std::expected<void, gisland::SupervisorCommandError> {
        return {};
      },
      "/run/user/1000/gisland.sock"};

  CHECK(std::holds_alternative<gisland::EmptyControlResult>(
      dispatcher.dispatch(gisland::OpenControl{}, now).value()));
  CHECK(mode.mode() == gisland::IslandMode::expanded);

  const auto status =
      std::get<gisland::ControlStatus>(dispatcher.dispatch(gisland::StatusControl{}, now).value());
  CHECK(status.mode == gisland::IslandMode::expanded);
  REQUIRE(status.active_context.has_value());
  CHECK(status.active_context->instance_id == "clock");
  CHECK(status.active_context->context_id == "configured");
  CHECK(status.modules == std::vector<gisland::ModuleControlStatus>{
                              {"clock", gisland::ControlModuleState::running, true},
                              {"status", gisland::ControlModuleState::stopped, false},
                              {"disabled", gisland::ControlModuleState::disabled, false}});
  CHECK(status.socket == "/run/user/1000/gisland.sock");

  CHECK(std::holds_alternative<gisland::EmptyControlResult>(
      dispatcher.dispatch(gisland::ToggleControl{}, now).value()));
  CHECK(mode.mode() == gisland::IslandMode::compact);
  CHECK(std::holds_alternative<gisland::EmptyControlResult>(
      dispatcher.dispatch(gisland::ToggleControl{}, now).value()));
  CHECK(mode.mode() == gisland::IslandMode::expanded);
  CHECK(std::holds_alternative<gisland::EmptyControlResult>(
      dispatcher.dispatch(gisland::CloseControl{}, now).value()));
  CHECK(mode.mode() == gisland::IslandMode::compact);
}

TEST_CASE("control dispatcher activation and dismissal update the next ordered query") {
  const gisland::MonotonicTime now{};
  gisland::RuntimeCoordinator runtime{config()};
  gisland::OverlayModeController mode;
  publish(runtime, "clock", "configured", 0, true, now);
  publish(runtime, "status", "alert", 100, false, now + 1ms);
  gisland::ControlDispatcher dispatcher{
      runtime, mode,
      [](std::string, std::uint64_t) -> std::expected<void, gisland::SupervisorCommandError> {
        return {};
      },
      "/tmp/gisland.sock"};

  REQUIRE(std::holds_alternative<gisland::EmptyControlResult>(
      dispatcher.dispatch(gisland::ActivateControl{"clock", 10ms}, now + 1ms).value()));
  auto status = std::get<gisland::ControlStatus>(
      dispatcher.dispatch(gisland::StatusControl{}, now + 1ms).value());
  REQUIRE(status.active_context.has_value());
  CHECK(status.active_context->instance_id == "clock");

  REQUIRE(std::holds_alternative<gisland::EmptyControlResult>(
      dispatcher.dispatch(gisland::DismissControl{"configured"}, now + 1ms).value()));
  status = std::get<gisland::ControlStatus>(
      dispatcher.dispatch(gisland::StatusControl{}, now + 1ms).value());
  REQUIRE(status.active_context.has_value());
  CHECK(status.active_context->instance_id == "status");
  CHECK(status.active_context->context_id == "alert");
}

TEST_CASE("control dispatcher rejects invalid domain operations without mutation") {
  const gisland::MonotonicTime now{};
  gisland::RuntimeCoordinator runtime{config()};
  gisland::OverlayModeController mode;
  publish(runtime, "clock", "compact-only", 0, false, now);
  gisland::ControlDispatcher dispatcher{
      runtime, mode,
      [](std::string, std::uint64_t) -> std::expected<void, gisland::SupervisorCommandError> {
        return {};
      },
      "/tmp/gisland.sock"};

  CHECK(error(dispatcher.dispatch(gisland::OpenControl{}, now)).code ==
        gisland::ControlErrorCode::unavailable_context);
  CHECK(mode.mode() == gisland::IslandMode::compact);
  CHECK(error(dispatcher.dispatch(gisland::ActivateControl{"missing", std::nullopt}, now)).code ==
        gisland::ControlErrorCode::unknown_instance);
  CHECK(error(dispatcher.dispatch(gisland::ActivateControl{"disabled", std::nullopt}, now)).code ==
        gisland::ControlErrorCode::unavailable_instance);
  CHECK(error(dispatcher.dispatch(gisland::ActivateControl{"status", std::nullopt}, now)).code ==
        gisland::ControlErrorCode::unavailable_instance);
  CHECK(error(dispatcher.dispatch(gisland::DismissControl{"other"}, now)).code ==
        gisland::ControlErrorCode::unknown_context);
  REQUIRE(runtime.active(now).context != nullptr);
  CHECK(runtime.active(now).context->key == gisland::ContextKey{"clock", "compact-only"});
}

TEST_CASE("control dispatcher correlates pending restart generations") {
  const gisland::MonotonicTime now{};
  gisland::RuntimeCoordinator runtime{config()};
  gisland::OverlayModeController mode;
  std::vector<std::pair<std::string, std::uint64_t>> restarts;
  bool reject = false;
  gisland::ControlDispatcher dispatcher{
      runtime, mode,
      [&restarts, &reject](std::string instance_id, std::uint64_t generation)
          -> std::expected<void, gisland::SupervisorCommandError> {
        if (reject) {
          return std::unexpected(gisland::SupervisorCommandError::shutting_down);
        }
        restarts.emplace_back(std::move(instance_id), generation);
        return {};
      },
      "/tmp/gisland.sock"};

  REQUIRE(std::holds_alternative<gisland::EmptyControlResult>(
      dispatcher.dispatch(gisland::RestartModuleControl{"clock"}, now).value()));
  REQUIRE(restarts.size() == 1);
  CHECK(restarts[0] == std::pair<std::string, std::uint64_t>{"clock", 1});
  CHECK(error(dispatcher.dispatch(gisland::RestartModuleControl{"clock"}, now)).code ==
        gisland::ControlErrorCode::restart_rejected);

  dispatcher.consume(
      gisland::RestartCompletedEvent{"clock", 99, true, gisland::ModuleState::running, now});
  CHECK(error(dispatcher.dispatch(gisland::RestartModuleControl{"clock"}, now)).code ==
        gisland::ControlErrorCode::restart_rejected);
  dispatcher.consume(
      gisland::RestartCompletedEvent{"clock", 1, false, gisland::ModuleState::failed, now});

  reject = true;
  CHECK(error(dispatcher.dispatch(gisland::RestartModuleControl{"clock"}, now)).code ==
        gisland::ControlErrorCode::restart_rejected);
  reject = false;
  REQUIRE(std::holds_alternative<gisland::EmptyControlResult>(
      dispatcher.dispatch(gisland::RestartModuleControl{"clock"}, now).value()));
  REQUIRE(restarts.size() == 2);
  CHECK(restarts[1] == std::pair<std::string, std::uint64_t>{"clock", 3});

  CHECK(error(dispatcher.dispatch(gisland::RestartModuleControl{"missing"}, now)).code ==
        gisland::ControlErrorCode::unknown_instance);
  CHECK(error(dispatcher.dispatch(gisland::RestartModuleControl{"disabled"}, now)).code ==
        gisland::ControlErrorCode::unavailable_instance);
}
