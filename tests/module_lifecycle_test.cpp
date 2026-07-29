#include "gisland/module_lifecycle.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>

namespace {

using namespace std::chrono_literals;

constexpr gisland::MonotonicTime epoch{};

void reach_running(gisland::ModuleLifecycle &lifecycle, gisland::MonotonicTime now) {
  REQUIRE(lifecycle.start(now).has_value());
  REQUIRE(lifecycle.ready(now).has_value());
  REQUIRE(lifecycle.state() == gisland::ModuleState::running);
}

} // namespace

TEST_CASE("module lifecycle accepts only valid public state transitions") {
  gisland::ModuleLifecycle lifecycle{gisland::RestartPolicy::on_failure, {}};

  CHECK(lifecycle.state() == gisland::ModuleState::stopped);
  REQUIRE_FALSE(lifecycle.ready(epoch).has_value());

  const auto started = lifecycle.start(epoch);
  REQUIRE(started.has_value());
  CHECK(started->from == gisland::ModuleState::stopped);
  CHECK(started->to == gisland::ModuleState::starting);
  REQUIRE_FALSE(lifecycle.start(epoch).has_value());

  const auto ready = lifecycle.ready(epoch + 1ms);
  REQUIRE(ready.has_value());
  CHECK(ready->from == gisland::ModuleState::starting);
  CHECK(ready->to == gisland::ModuleState::running);
  REQUIRE_FALSE(lifecycle.ready(epoch + 1ms).has_value());

  const auto stopping = lifecycle.stop(epoch + 2ms);
  REQUIRE(stopping.has_value());
  CHECK(stopping->from == gisland::ModuleState::running);
  CHECK(stopping->to == gisland::ModuleState::stopping);
  CHECK(stopping->cause == gisland::StopCause::requested);
  REQUIRE_FALSE(lifecycle.stop(epoch + 2ms).has_value());

  const auto exited = lifecycle.exited(gisland::StopCause::clean_exit, epoch + 3ms);
  REQUIRE(exited.size() == 1);
  CHECK(exited.front().from == gisland::ModuleState::stopping);
  CHECK(exited.front().to == gisland::ModuleState::stopped);
  CHECK(exited.front().cause == gisland::StopCause::requested);
}

TEST_CASE("handshake and shutdown deadlines use only injected monotonic time") {
  gisland::ModuleLifecycle lifecycle{gisland::RestartPolicy::on_failure, {}};
  REQUIRE(lifecycle.start(epoch).has_value());

  CHECK(lifecycle.tick(epoch + 1999ms).empty());
  const auto timeout = lifecycle.tick(epoch + 2s);
  REQUIRE(timeout.size() == 1);
  CHECK(timeout.front().from == gisland::ModuleState::starting);
  CHECK(timeout.front().to == gisland::ModuleState::stopping);
  CHECK(timeout.front().cause == gisland::StopCause::handshake_timeout);
  REQUIRE(lifecycle.due_signal(epoch + 2s).has_value());
  CHECK(*lifecycle.due_signal(epoch + 2s) == gisland::ShutdownSignal::terminate);

  REQUIRE(lifecycle.signal_sent(gisland::ShutdownSignal::terminate, epoch + 2s).has_value());
  CHECK_FALSE(lifecycle.due_signal(epoch + 2499ms).has_value());
  REQUIRE(lifecycle.due_signal(epoch + 2500ms).has_value());
  CHECK(*lifecycle.due_signal(epoch + 2500ms) == gisland::ShutdownSignal::kill);

  REQUIRE(lifecycle.signal_sent(gisland::ShutdownSignal::kill, epoch + 2500ms).has_value());
  CHECK_FALSE(lifecycle.due_signal(epoch + 3s).has_value());

  const auto exited = lifecycle.exited(gisland::StopCause::signal, epoch + 2501ms);
  REQUIRE(exited.size() == 1);
  CHECK(exited.front().to == gisland::ModuleState::backoff);
  CHECK(exited.front().cause == gisland::StopCause::handshake_timeout);
}

TEST_CASE("requested shutdown waits before escalating to the process group") {
  gisland::ModuleLifecycle lifecycle{gisland::RestartPolicy::always, {}};
  reach_running(lifecycle, epoch);

  REQUIRE(lifecycle.stop(epoch).has_value());
  CHECK_FALSE(lifecycle.due_signal(epoch + 999ms).has_value());
  REQUIRE(lifecycle.due_signal(epoch + 1s).has_value());
  CHECK(*lifecycle.due_signal(epoch + 1s) == gisland::ShutdownSignal::terminate);

  REQUIRE(lifecycle.signal_sent(gisland::ShutdownSignal::terminate, epoch + 1s).has_value());
  CHECK_FALSE(lifecycle.due_signal(epoch + 1499ms).has_value());
  REQUIRE(lifecycle.due_signal(epoch + 1500ms).has_value());
  CHECK(*lifecycle.due_signal(epoch + 1500ms) == gisland::ShutdownSignal::kill);

  const auto exited = lifecycle.exited(gisland::StopCause::signal, epoch + 1501ms);
  REQUIRE(exited.size() == 1);
  CHECK(exited.front().to == gisland::ModuleState::stopped);
  CHECK(exited.front().cause == gisland::StopCause::requested);
}

TEST_CASE("restart policies distinguish clean and failed exits") {
  SECTION("always restarts every unexpected exit") {
    for (const auto cause :
         {gisland::StopCause::clean_exit, gisland::StopCause::failed_exit,
          gisland::StopCause::signal}) {
      gisland::ModuleLifecycle lifecycle{gisland::RestartPolicy::always, {}};
      reach_running(lifecycle, epoch);
      const auto transitions = lifecycle.exited(cause, epoch + 1ms);
      REQUIRE(transitions.size() == 1);
      CHECK(transitions.front().to == gisland::ModuleState::backoff);
    }
  }

  SECTION("on-failure restarts only failed exits") {
    gisland::ModuleLifecycle clean{gisland::RestartPolicy::on_failure, {}};
    reach_running(clean, epoch);
    REQUIRE(clean.exited(gisland::StopCause::clean_exit, epoch + 1ms).size() == 1);
    CHECK(clean.state() == gisland::ModuleState::stopped);

    for (const auto cause : {gisland::StopCause::failed_exit, gisland::StopCause::signal,
                             gisland::StopCause::spawn_error,
                             gisland::StopCause::protocol_violation,
                             gisland::StopCause::io_error, gisland::StopCause::unresponsive}) {
      gisland::ModuleLifecycle failed{gisland::RestartPolicy::on_failure, {}};
      reach_running(failed, epoch);
      REQUIRE(failed.exited(cause, epoch + 1ms).size() == 1);
      CHECK(failed.state() == gisland::ModuleState::backoff);
    }
  }

  SECTION("never stops after every exit") {
    for (const auto cause :
         {gisland::StopCause::clean_exit, gisland::StopCause::failed_exit,
          gisland::StopCause::signal}) {
      gisland::ModuleLifecycle lifecycle{gisland::RestartPolicy::never, {}};
      reach_running(lifecycle, epoch);
      REQUIRE(lifecycle.exited(cause, epoch + 1ms).size() == 1);
      CHECK(lifecycle.state() == gisland::ModuleState::stopped);
    }
  }
}

TEST_CASE("restart backoff doubles to its cap without sleeping") {
  gisland::ModuleLifecycle lifecycle{gisland::RestartPolicy::on_failure, {}};
  constexpr std::array expected_delays{250ms, 500ms, 1000ms, 2000ms, 4000ms,
                                       8000ms, 16000ms, 30000ms, 30000ms};
  auto now = epoch;

  for (const auto expected_delay : expected_delays) {
    if (lifecycle.state() == gisland::ModuleState::stopped) {
      reach_running(lifecycle, now);
    } else {
      REQUIRE(lifecycle.state() == gisland::ModuleState::backoff);
      REQUIRE(lifecycle.restart_at().has_value());
      now = *lifecycle.restart_at();
      const auto restarted = lifecycle.tick(now);
      REQUIRE(restarted.size() == 1);
      CHECK(restarted.front().to == gisland::ModuleState::starting);
      REQUIRE(lifecycle.ready(now).has_value());
    }

    const auto transitions = lifecycle.exited(gisland::StopCause::failed_exit, now);
    REQUIRE(transitions.size() == 1);
    REQUIRE(lifecycle.restart_at().has_value());
    CHECK(*lifecycle.restart_at() - now == expected_delay);
  }
}

TEST_CASE("a healthy run resets exponential backoff") {
  gisland::ModuleLifecycle lifecycle{gisland::RestartPolicy::on_failure, {}};
  reach_running(lifecycle, epoch);

  REQUIRE(lifecycle.exited(gisland::StopCause::failed_exit, epoch).size() == 1);
  REQUIRE(lifecycle.restart_at().has_value());
  CHECK(*lifecycle.restart_at() - epoch == 250ms);

  auto now = *lifecycle.restart_at();
  REQUIRE(lifecycle.tick(now).size() == 1);
  REQUIRE(lifecycle.ready(now).has_value());
  REQUIRE(lifecycle.exited(gisland::StopCause::failed_exit, now + 1s).size() == 1);
  REQUIRE(lifecycle.restart_at().has_value());
  CHECK(*lifecycle.restart_at() - (now + 1s) == 500ms);

  now = *lifecycle.restart_at();
  REQUIRE(lifecycle.tick(now).size() == 1);
  REQUIRE(lifecycle.ready(now).has_value());
  CHECK(lifecycle.tick(now + 60s).empty());
  REQUIRE(lifecycle.exited(gisland::StopCause::failed_exit, now + 60s).size() == 1);
  REQUIRE(lifecycle.restart_at().has_value());
  CHECK(*lifecycle.restart_at() - (now + 60s) == 250ms);
}

TEST_CASE("ten rolling failures lock the instance until explicit start") {
  gisland::ModuleLifecycle lifecycle{gisland::RestartPolicy::on_failure, {}};
  auto now = epoch;

  for (int failure = 1; failure <= 10; ++failure) {
    if (lifecycle.state() == gisland::ModuleState::stopped) {
      reach_running(lifecycle, now);
    } else {
      REQUIRE(lifecycle.restart_at().has_value());
      now = *lifecycle.restart_at();
      REQUIRE(lifecycle.tick(now).size() == 1);
      REQUIRE(lifecycle.ready(now).has_value());
    }
    REQUIRE(lifecycle.exited(gisland::StopCause::failed_exit, now).size() == 1);
    if (failure < 10) {
      CHECK(lifecycle.state() == gisland::ModuleState::backoff);
    }
  }

  CHECK(lifecycle.state() == gisland::ModuleState::failed);
  CHECK_FALSE(lifecycle.restart_at().has_value());

  const auto manual_start = lifecycle.start(now + 1ms);
  REQUIRE(manual_start.has_value());
  CHECK(manual_start->from == gisland::ModuleState::failed);
  CHECK(manual_start->to == gisland::ModuleState::starting);
}

TEST_CASE("failure lockout uses a rolling five-minute window") {
  gisland::ModuleLifecycle lifecycle{gisland::RestartPolicy::on_failure, {}};
  auto now = epoch;

  for (int failure = 0; failure < 9; ++failure) {
    if (lifecycle.state() == gisland::ModuleState::stopped) {
      reach_running(lifecycle, now);
    } else {
      REQUIRE(lifecycle.restart_at().has_value());
      now = *lifecycle.restart_at();
      REQUIRE(lifecycle.tick(now).size() == 1);
      REQUIRE(lifecycle.ready(now).has_value());
    }
    REQUIRE(lifecycle.exited(gisland::StopCause::failed_exit, now).size() == 1);
  }

  REQUIRE(lifecycle.restart_at().has_value());
  now = *lifecycle.restart_at();
  REQUIRE(lifecycle.tick(now).size() == 1);
  REQUIRE(lifecycle.ready(now).has_value());
  now = epoch + 5min + 1ms;
  REQUIRE(lifecycle.exited(gisland::StopCause::failed_exit, now).size() == 1);
  CHECK(lifecycle.state() == gisland::ModuleState::backoff);
}
