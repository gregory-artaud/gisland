#include "gisland/control_dispatcher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <tuple>
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

void ready(gisland::RuntimeCoordinator &runtime, std::string instance_id, std::uint64_t generation,
           int protocol_minor, gisland::MonotonicTime now) {
  REQUIRE(runtime.consume(gisland::ProcessStartedEvent{instance_id, 100, 100, now, generation})
              .has_value());
  REQUIRE(runtime
              .consume(gisland::ModuleMessageEvent{std::move(instance_id),
                                                   gisland::ReadyMessage{1, protocol_minor, {}},
                                                   now, generation})
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
  REQUIRE(status.compact.has_value());
  REQUIRE(status.expanded.has_value());
  CHECK(status.compact->instance_id == "clock");
  CHECK(status.expanded->context_id == "configured");
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
  REQUIRE(status.compact.has_value());
  CHECK(status.compact->instance_id == "clock");

  REQUIRE(std::holds_alternative<gisland::EmptyControlResult>(
      dispatcher.dispatch(gisland::DismissControl{"configured"}, now + 1ms).value()));
  status = std::get<gisland::ControlStatus>(
      dispatcher.dispatch(gisland::StatusControl{}, now + 1ms).value());
  REQUIRE(status.compact.has_value());
  CHECK(status.compact->instance_id == "status");
  CHECK(status.compact->context_id == "alert");
}

TEST_CASE("successful activation closes an open overlay") {
  const gisland::MonotonicTime now{};
  gisland::RuntimeCoordinator runtime{config()};
  gisland::OverlayModeController mode;
  publish(runtime, "clock", "configured", 0, true, now);
  gisland::ControlDispatcher dispatcher{
      runtime, mode,
      [](std::string, std::uint64_t) -> std::expected<void, gisland::SupervisorCommandError> {
        return {};
      },
      "/tmp/gisland.sock"};

  REQUIRE(std::holds_alternative<gisland::EmptyControlResult>(
      dispatcher.dispatch(gisland::OpenControl{}, now).value()));
  REQUIRE(mode.mode() == gisland::IslandMode::expanded);

  REQUIRE(std::holds_alternative<gisland::EmptyControlResult>(
      dispatcher.dispatch(gisland::ActivateControl{"clock", 3s}, now).value()));
  CHECK(mode.mode() == gisland::IslandMode::compact);
}

TEST_CASE("activate-open atomically selects and opens the expanded owner") {
  const gisland::MonotonicTime now{};
  gisland::RuntimeCoordinator runtime{config()};
  gisland::OverlayModeController mode;
  publish(runtime, "clock", "configured", 0, true, now);
  publish(runtime, "status", "history", 100, true, now + 1ms);
  gisland::ControlDispatcher dispatcher{
      runtime, mode,
      [](std::string, std::uint64_t) -> std::expected<void, gisland::SupervisorCommandError> {
        return {};
      },
      "/tmp/gisland.sock"};

  REQUIRE(std::holds_alternative<gisland::EmptyControlResult>(
      dispatcher.dispatch(gisland::ActivateOpenControl{"status"}, now + 1ms).value()));
  CHECK(mode.mode() == gisland::IslandMode::expanded);
  const auto status = std::get<gisland::ControlStatus>(
      dispatcher.dispatch(gisland::StatusControl{}, now + 1ms).value());
  REQUIRE(status.expanded.has_value());
  CHECK(status.expanded->instance_id == "status");
  CHECK(status.expanded->context_id == "history");
}

TEST_CASE("control dispatcher opens and dismisses the independently selected expanded owner") {
  const gisland::MonotonicTime now{};
  gisland::RuntimeCoordinator runtime{config()};
  gisland::OverlayModeController mode;
  publish(runtime, "clock", "configured", 0, false, now);
  REQUIRE(
      runtime
          .consume(gisland::ModuleMessageEvent{"status",
                                               gisland::PublishMessage{.context_id = "alert",
                                                                       .priority = 10,
                                                                       .expanded = text("expanded"),
                                                                       .independent_views = true},
                                               now})
          .has_value());
  gisland::ControlDispatcher dispatcher{
      runtime, mode,
      [](std::string, std::uint64_t) -> std::expected<void, gisland::SupervisorCommandError> {
        return {};
      },
      "/tmp/gisland.sock"};

  CHECK(std::holds_alternative<gisland::EmptyControlResult>(
      dispatcher.dispatch(gisland::OpenControl{}, now).value()));
  CHECK(mode.mode() == gisland::IslandMode::expanded);
  CHECK(std::holds_alternative<gisland::EmptyControlResult>(
      dispatcher.dispatch(gisland::DismissControl{"alert"}, now).value()));

  const auto status =
      std::get<gisland::ControlStatus>(dispatcher.dispatch(gisland::StatusControl{}, now).value());
  REQUIRE(status.compact.has_value());
  CHECK(status.compact->instance_id == "clock");
  CHECK_FALSE(status.expanded.has_value());
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
  CHECK(mode.mode() == gisland::IslandMode::compact);
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

TEST_CASE(
    "control dispatcher reports reload rejection and exposes successful mutation immediately") {
  const gisland::MonotonicTime now{};
  auto application = config();
  gisland::RuntimeCoordinator runtime{application};
  gisland::OverlayModeController mode;
  bool reject = true;
  gisland::ControlDispatcher dispatcher{
      runtime, mode,
      [](std::string, std::uint64_t) -> std::expected<void, gisland::SupervisorCommandError> {
        return {};
      },
      "/tmp/gisland.sock",
      [&](gisland::MonotonicTime) -> std::expected<void, std::string> {
        if (reject) {
          return std::unexpected("candidate configuration is invalid");
        }
        auto candidate = application;
        candidate.modules[2].enabled = true;
        candidate.modules = {candidate.modules[2], candidate.modules[0], candidate.modules[1]};
        const auto plan = gisland::plan_reload(application, candidate, "C", "UTC");
        if (!plan) {
          return std::unexpected(plan.error().message);
        }
        auto prepared = runtime.prepare_reload(*plan);
        if (!prepared) {
          return std::unexpected(prepared.error().message);
        }
        runtime.commit_reload(std::move(*prepared));
        application = std::move(candidate);
        return {};
      }};

  const auto rejected = dispatcher.dispatch(gisland::ReloadControl{}, now);
  REQUIRE(std::holds_alternative<gisland::ControlError>(rejected.value()));
  CHECK(error(rejected).code == gisland::ControlErrorCode::reload_rejected);
  CHECK(error(rejected).message == "candidate configuration is invalid");
  CHECK(runtime.module_statuses(now)[0].id == "clock");

  reject = false;
  CHECK(std::holds_alternative<gisland::EmptyControlResult>(
      dispatcher.dispatch(gisland::ReloadControl{}, now).value()));
  const auto status =
      std::get<gisland::ControlStatus>(dispatcher.dispatch(gisland::StatusControl{}, now).value());
  REQUIRE(status.modules.size() == 3);
  CHECK(status.modules[0] ==
        gisland::ModuleControlStatus{"disabled", gisland::ControlModuleState::stopped, false});
}

TEST_CASE("control dispatcher registers correlated actions against the current ready generation") {
  const gisland::MonotonicTime now{};
  gisland::RuntimeCoordinator runtime{config()};
  gisland::OverlayModeController mode;
  ready(runtime, "clock", 7, 8, now);
  std::vector<std::tuple<std::string, std::uint64_t, gisland::ActionMessage>> sent;
  gisland::ControlDispatcher dispatcher{
      runtime,
      mode,
      [](std::string, std::uint64_t) -> std::expected<void, gisland::SupervisorCommandError> {
        return {};
      },
      "/tmp/gisland.sock",
      {},
      [&sent](std::string instance, std::uint64_t generation, gisland::ActionMessage action) {
        sent.emplace_back(std::move(instance), generation, std::move(action));
        return std::expected<void, gisland::SupervisorCommandError>{};
      }};

  const auto dispatched = dispatcher.dispatch_deferred(
      gisland::ActionControl{"clock", "set", nlohmann::json(nullptr)}, now);
  REQUIRE(std::holds_alternative<gisland::PendingControlToken>(dispatched));
  const auto token = std::get<gisland::PendingControlToken>(dispatched);
  REQUIRE(sent.size() == 1);
  CHECK(std::get<0>(sent[0]) == "clock");
  CHECK(std::get<1>(sent[0]) == 7);
  CHECK(std::get<2>(sent[0]).action_id == "set");
  REQUIRE(std::get<2>(sent[0]).invocation_id.has_value());
  CHECK(*std::get<2>(sent[0]).invocation_id == token.value);
  REQUIRE(std::get<2>(sent[0]).value.has_value());
  CHECK(std::get<2>(sent[0]).value->is_null());
  CHECK(dispatcher.pending_action_count() == 1);
}

TEST_CASE("control dispatcher rejects unavailable and pre-1.8 action targets immediately") {
  const gisland::MonotonicTime now{};
  gisland::RuntimeCoordinator runtime{config()};
  gisland::OverlayModeController mode;
  ready(runtime, "clock", 4, 7, now);
  gisland::ControlDispatcher dispatcher{
      runtime,
      mode,
      [](std::string, std::uint64_t) -> std::expected<void, gisland::SupervisorCommandError> {
        return {};
      },
      "/tmp/gisland.sock",
      {},
      [](std::string, std::uint64_t, gisland::ActionMessage) {
        return std::expected<void, gisland::SupervisorCommandError>{};
      }};

  const auto old = dispatcher.dispatch_deferred(gisland::ActionControl{"clock", "set", {}}, now);
  REQUIRE(std::holds_alternative<gisland::ControlResponse>(old));
  CHECK(error(std::get<gisland::ControlResponse>(old)).code ==
        gisland::ControlErrorCode::unsupported_module_protocol);
  const auto stopped =
      dispatcher.dispatch_deferred(gisland::ActionControl{"status", "set", {}}, now);
  REQUIRE(std::holds_alternative<gisland::ControlResponse>(stopped));
  CHECK(error(std::get<gisland::ControlResponse>(stopped)).code ==
        gisland::ControlErrorCode::unavailable_instance);
  const auto missing =
      dispatcher.dispatch_deferred(gisland::ActionControl{"missing", "set", {}}, now);
  REQUIRE(std::holds_alternative<gisland::ControlResponse>(missing));
  CHECK(error(std::get<gisland::ControlResponse>(missing)).code ==
        gisland::ControlErrorCode::unknown_instance);
}

TEST_CASE("action completion requires delivery and exact generation invocation and action") {
  const gisland::MonotonicTime now{};
  gisland::RuntimeCoordinator runtime{config()};
  gisland::OverlayModeController mode;
  ready(runtime, "clock", 9, 8, now);
  gisland::ControlDispatcher dispatcher{
      runtime,
      mode,
      [](std::string, std::uint64_t) -> std::expected<void, gisland::SupervisorCommandError> {
        return {};
      },
      "/tmp/gisland.sock",
      {},
      [](std::string, std::uint64_t, gisland::ActionMessage) {
        return std::expected<void, gisland::SupervisorCommandError>{};
      }};
  const auto request = dispatcher.dispatch_deferred(gisland::ActionControl{"clock", "set", 3}, now);
  const auto token = std::get<gisland::PendingControlToken>(request);

  dispatcher.expire(now + 10s);
  CHECK(dispatcher.take_completed().empty());
  dispatcher.consume(gisland::ActionDeliveryEvent{"clock", 8, token.value, true, {}, now});
  dispatcher.consume(gisland::ActionDeliveryEvent{"clock", 9, token.value, true, {}, now + 1s});
  CHECK(dispatcher.consume(gisland::ModuleMessageEvent{
            "clock", gisland::ActionResultMessage{"other", true, {}, token.value}, now + 1100ms,
            9}) == gisland::ActionEventResult::stale);
  CHECK(dispatcher.consume(gisland::ModuleMessageEvent{
            "clock", gisland::ActionResultMessage{"set", true, {}, token.value + 1}, now + 1200ms,
            9}) == gisland::ActionEventResult::stale);
  CHECK(dispatcher.take_completed().empty());
  CHECK(dispatcher.consume(gisland::ModuleMessageEvent{
            "clock", gisland::ActionResultMessage{"set", true, {}, token.value}, now + 1300ms,
            9}) == gisland::ActionEventResult::consumed);
  CHECK(dispatcher.consume(gisland::ModuleMessageEvent{
            "clock", gisland::ActionResultMessage{"set", false, "late", token.value}, now + 1400ms,
            9}) == gisland::ActionEventResult::stale);
  const auto completed = dispatcher.take_completed();
  REQUIRE(completed.size() == 1);
  CHECK(completed[0].token == token);
  CHECK(std::holds_alternative<gisland::EmptyControlResult>(completed[0].response.value()));
}

TEST_CASE("correlated action results missing invocation IDs are protocol errors") {
  const gisland::MonotonicTime now{};
  gisland::RuntimeCoordinator runtime{config()};
  gisland::OverlayModeController mode;
  ready(runtime, "clock", 9, 8, now);
  gisland::ControlDispatcher dispatcher{
      runtime,
      mode,
      [](std::string, std::uint64_t) -> std::expected<void, gisland::SupervisorCommandError> {
        return {};
      },
      "/tmp/gisland.sock",
      {},
      [](std::string, std::uint64_t, gisland::ActionMessage) {
        return std::expected<void, gisland::SupervisorCommandError>{};
      }};
  const auto token = std::get<gisland::PendingControlToken>(
      dispatcher.dispatch_deferred(gisland::ActionControl{"clock", "set", {}}, now));
  dispatcher.consume(gisland::ActionDeliveryEvent{"clock", 9, token.value, true, {}, now});
  CHECK(dispatcher.consume(gisland::ModuleMessageEvent{
            "clock", gisland::ActionResultMessage{"set", true, {}, std::nullopt}, now, 9}) ==
        gisland::ActionEventResult::protocol_error);
  CHECK(dispatcher.pending_action_count() == 1);
}

TEST_CASE("action rejection delivery failure and delivered timeout complete once") {
  const gisland::MonotonicTime now{};
  gisland::RuntimeCoordinator runtime{config()};
  gisland::OverlayModeController mode;
  ready(runtime, "clock", 2, 8, now);
  gisland::ControlDispatcher dispatcher{
      runtime,
      mode,
      [](std::string, std::uint64_t) -> std::expected<void, gisland::SupervisorCommandError> {
        return {};
      },
      "/tmp/gisland.sock",
      {},
      [](std::string, std::uint64_t, gisland::ActionMessage) {
        return std::expected<void, gisland::SupervisorCommandError>{};
      }};

  const auto rejected = std::get<gisland::PendingControlToken>(
      dispatcher.dispatch_deferred(gisland::ActionControl{"clock", "reject", {}}, now));
  dispatcher.consume(gisland::ActionDeliveryEvent{"clock", 2, rejected.value, true, {}, now});
  CHECK(dispatcher.consume(gisland::ModuleMessageEvent{
            "clock", gisland::ActionResultMessage{"reject", false, "no", rejected.value}, now,
            2}) == gisland::ActionEventResult::consumed);

  const auto failed = std::get<gisland::PendingControlToken>(
      dispatcher.dispatch_deferred(gisland::ActionControl{"clock", "fail", {}}, now));
  dispatcher.consume(gisland::ActionDeliveryEvent{
      "clock", 2, failed.value, false, gisland::ActionDeliveryError::queue_saturated, now});

  const auto timed_out = std::get<gisland::PendingControlToken>(
      dispatcher.dispatch_deferred(gisland::ActionControl{"clock", "wait", {}}, now));
  dispatcher.consume(gisland::ActionDeliveryEvent{"clock", 2, timed_out.value, true, {}, now + 1s});
  dispatcher.expire(now + 2999ms);
  CHECK(dispatcher.take_completed().size() == 2);
  dispatcher.expire(now + 3s);
  const auto timeout = dispatcher.take_completed();
  REQUIRE(timeout.size() == 1);
  CHECK(timeout[0].token == timed_out);
  CHECK(error(timeout[0].response).code == gisland::ControlErrorCode::action_timeout);
}

TEST_CASE("action IDs wrap and skip live identifiers") {
  const gisland::MonotonicTime now{};
  gisland::RuntimeCoordinator runtime{config()};
  gisland::OverlayModeController mode;
  ready(runtime, "clock", 3, 8, now);
  gisland::ControlDispatcher dispatcher{
      runtime,
      mode,
      [](std::string, std::uint64_t) -> std::expected<void, gisland::SupervisorCommandError> {
        return {};
      },
      "/tmp/gisland.sock",
      {},
      [](std::string, std::uint64_t, gisland::ActionMessage) {
        return std::expected<void, gisland::SupervisorCommandError>{};
      },
      std::numeric_limits<std::uint64_t>::max()};
  const auto maximum = std::get<gisland::PendingControlToken>(
      dispatcher.dispatch_deferred(gisland::ActionControl{"clock", "a", {}}, now));
  const auto one = std::get<gisland::PendingControlToken>(
      dispatcher.dispatch_deferred(gisland::ActionControl{"clock", "b", {}}, now));
  CHECK(maximum.value == std::numeric_limits<std::uint64_t>::max());
  CHECK(one.value == 1);
}

TEST_CASE("explicit action cancellation isolates tokens generations instances and shutdown") {
  const gisland::MonotonicTime now{};
  gisland::RuntimeCoordinator runtime{config()};
  gisland::OverlayModeController mode;
  ready(runtime, "clock", 5, 8, now);
  ready(runtime, "status", 6, 8, now);
  gisland::ControlDispatcher dispatcher{
      runtime,
      mode,
      [](std::string, std::uint64_t) -> std::expected<void, gisland::SupervisorCommandError> {
        return {};
      },
      "/tmp/gisland.sock",
      {},
      [](std::string, std::uint64_t, gisland::ActionMessage) {
        return std::expected<void, gisland::SupervisorCommandError>{};
      }};
  const auto client = std::get<gisland::PendingControlToken>(
      dispatcher.dispatch_deferred(gisland::ActionControl{"clock", "client", {}}, now));
  static_cast<void>(dispatcher.dispatch_deferred(gisland::ActionControl{"clock", "exit", {}}, now));
  static_cast<void>(
      dispatcher.dispatch_deferred(gisland::ActionControl{"status", "disable", {}}, now));
  CHECK(dispatcher.cancel(client));
  dispatcher.cancel_generation("clock", 4);
  CHECK(dispatcher.pending_action_count() == 2);
  dispatcher.consume(
      gisland::ProcessExitedEvent{"clock", 1, {}, gisland::StopCause::failed_exit, now, 5});
  CHECK(dispatcher.pending_action_count() == 1);
  dispatcher.cancel_instance("status");
  CHECK(dispatcher.pending_action_count() == 0);
  CHECK(dispatcher.take_completed().size() == 2);

  static_cast<void>(
      dispatcher.dispatch_deferred(gisland::ActionControl{"clock", "reload", {}}, now));
  static_cast<void>(
      dispatcher.dispatch_deferred(gisland::ActionControl{"status", "shutdown", {}}, now));
  dispatcher.cancel_all();
  CHECK(dispatcher.pending_action_count() == 0);
  CHECK(dispatcher.take_completed().size() == 2);
}

TEST_CASE("context removal and replacement start cancel only their owned action generation") {
  const gisland::MonotonicTime now{};
  gisland::RuntimeCoordinator runtime{config()};
  gisland::OverlayModeController mode;
  ready(runtime, "clock", 11, 8, now);
  ready(runtime, "status", 12, 8, now);
  gisland::ControlDispatcher dispatcher{
      runtime,
      mode,
      [](std::string, std::uint64_t) -> std::expected<void, gisland::SupervisorCommandError> {
        return {};
      },
      "/tmp/gisland.sock",
      {},
      [](std::string, std::uint64_t, gisland::ActionMessage) {
        return std::expected<void, gisland::SupervisorCommandError>{};
      }};
  static_cast<void>(
      dispatcher.dispatch_deferred(gisland::ActionControl{"clock", "removed", {}}, now));
  static_cast<void>(
      dispatcher.dispatch_deferred(gisland::ActionControl{"status", "replaced", {}}, now));

  dispatcher.consume(gisland::ContextsRemovedEvent{"clock", now, 10});
  CHECK(dispatcher.pending_action_count() == 2);
  dispatcher.consume(gisland::ContextsRemovedEvent{"clock", now, 11});
  CHECK(dispatcher.pending_action_count() == 1);
  dispatcher.consume(gisland::ProcessStartedEvent{"status", 2, 2, now, 13});
  CHECK(dispatcher.pending_action_count() == 0);
  CHECK(dispatcher.take_completed().size() == 2);
}
