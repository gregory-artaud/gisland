#include "gisland/config.hpp"
#include "gisland/context.hpp"
#include "gisland/module_supervisor.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#ifndef GISLAND_FAKE_MODULE_PATH
#error "GISLAND_FAKE_MODULE_PATH must name the integration-test helper"
#endif

namespace {

using namespace std::chrono_literals;

using EventLog = std::vector<gisland::SupervisorEvent>;

[[nodiscard]] gisland::ModuleTimings fast_timings() {
  return gisland::ModuleTimings{
      .handshake = 500ms,
      .graceful_shutdown = 50ms,
      .terminate_grace = 30ms,
      .initial_backoff = 20ms,
      .maximum_backoff = 80ms,
      .healthy_reset = 200ms,
  };
}

[[nodiscard]] gisland::ModuleStartRequest
fake_request(std::string instance_id, std::string mode,
             gisland::RestartPolicy restart = gisland::RestartPolicy::never,
             gisland::ModuleTimings timings = fast_timings()) {
  return gisland::ModuleStartRequest{
      .instance_id = std::move(instance_id),
      .process =
          {
              .argv = {GISLAND_FAKE_MODULE_PATH, std::move(mode)},
              .environment = {},
              .working_directory = std::nullopt,
          },
      .init =
          {
              .minimum = {.major = 1, .minor = 0},
              .maximum = {.major = 1, .minor = 0},
              .instance_id = "caller-value-is-not-authoritative",
              .capabilities = {"actions", "visibility"},
              .configuration = nlohmann::json::object(),
              .locale = "C",
              .timezone = "UTC",
          },
      .restart = restart,
      .timings = timings,
  };
}

template <typename Event>
[[nodiscard]] std::size_t count_events(const EventLog &events, std::string_view instance_id) {
  return static_cast<std::size_t>(std::ranges::count_if(events, [instance_id](const auto &event) {
    const auto *typed = std::get_if<Event>(&event);
    return typed != nullptr && typed->instance_id == instance_id;
  }));
}

[[nodiscard]] bool has_state(const EventLog &events, std::string_view instance_id,
                             gisland::ModuleState state) {
  return std::ranges::any_of(events, [instance_id, state](const auto &event) {
    const auto *changed = std::get_if<gisland::StateChangedEvent>(&event);
    return changed != nullptr && changed->instance_id == instance_id &&
           changed->transition.to == state;
  });
}

[[nodiscard]] bool has_state_cause(const EventLog &events, std::string_view instance_id,
                                   gisland::ModuleState state, gisland::StopCause cause) {
  return std::ranges::any_of(events, [instance_id, state, cause](const auto &event) {
    const auto *changed = std::get_if<gisland::StateChangedEvent>(&event);
    return changed != nullptr && changed->instance_id == instance_id &&
           changed->transition.to == state && changed->transition.cause == cause;
  });
}

template <typename Message>
[[nodiscard]] bool has_message(const EventLog &events, std::string_view instance_id) {
  return std::ranges::any_of(events, [instance_id](const auto &event) {
    const auto *message = std::get_if<gisland::ModuleMessageEvent>(&event);
    return message != nullptr && message->instance_id == instance_id &&
           std::holds_alternative<Message>(message->message);
  });
}

[[nodiscard]] const gisland::ActionDeliveryEvent *delivery(const EventLog &events,
                                                           std::uint64_t invocation_id) {
  const auto found = std::ranges::find_if(events, [invocation_id](const auto &event) {
    const auto *typed = std::get_if<gisland::ActionDeliveryEvent>(&event);
    return typed != nullptr && typed->invocation_id == invocation_id;
  });
  return found == events.end() ? nullptr : std::get_if<gisland::ActionDeliveryEvent>(&*found);
}

[[nodiscard]] std::uint64_t process_generation(const EventLog &events, std::string_view instance_id,
                                               std::size_t ordinal = 0) {
  std::size_t found_count = 0;
  for (const auto &event : events) {
    const auto *started = std::get_if<gisland::ProcessStartedEvent>(&event);
    if (started != nullptr && started->instance_id == instance_id && found_count++ == ordinal) {
      return started->generation;
    }
  }
  return 0;
}

[[nodiscard]] const gisland::ProtocolViolationEvent *last_violation(const EventLog &events,
                                                                    std::string_view instance_id) {
  const auto found =
      std::ranges::find_if(events.rbegin(), events.rend(), [instance_id](const auto &event) {
        const auto *violation = std::get_if<gisland::ProtocolViolationEvent>(&event);
        return violation != nullptr && violation->instance_id == instance_id;
      });
  return found == events.rend() ? nullptr : std::get_if<gisland::ProtocolViolationEvent>(&*found);
}

void collect_until(gisland::ModuleSupervisor &supervisor, EventLog &events,
                   const std::function<bool(const EventLog &)> &condition,
                   std::chrono::milliseconds timeout = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!condition(events) && std::chrono::steady_clock::now() < deadline) {
    auto next = supervisor.wait_for_events(50ms);
    events.insert(events.end(), std::make_move_iterator(next.begin()),
                  std::make_move_iterator(next.end()));
  }
  REQUIRE(condition(events));
}

void stop_and_wait(gisland::ModuleSupervisor &supervisor, EventLog &events,
                   std::string_view instance_id) {
  REQUIRE(supervisor.stop(std::string{instance_id}).has_value());
  collect_until(supervisor, events, [instance_id](const auto &observed) {
    return has_state(observed, instance_id, gisland::ModuleState::stopped);
  });
}

[[nodiscard]] std::optional<std::size_t>
event_index(const EventLog &events,
            const std::function<bool(const gisland::SupervisorEvent &)> &is) {
  for (std::size_t index = 0; index < events.size(); ++index) {
    if (is(events[index])) {
      return index;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::size_t open_descriptor_count() {
  return static_cast<std::size_t>(std::distance(
      std::filesystem::directory_iterator{"/proc/self/fd"}, std::filesystem::directory_iterator{}));
}

} // namespace

TEST_CASE("supervisor asynchronously handshakes publishes and gracefully stops") {
  gisland::ModuleSupervisor supervisor;
  EventLog events;
  const auto before_start = std::chrono::steady_clock::now();
  REQUIRE(supervisor.start(fake_request("owner", "publish")).has_value());
  CHECK(std::chrono::steady_clock::now() - before_start < 100ms);

  collect_until(supervisor, events, [](const auto &observed) {
    return has_state(observed, "owner", gisland::ModuleState::running) &&
           has_message<gisland::PublishMessage>(observed, "owner");
  });

  CHECK(has_state(events, "owner", gisland::ModuleState::starting));
  CHECK(count_events<gisland::ProcessStartedEvent>(events, "owner") == 1);
  const auto generation = process_generation(events, "owner");
  REQUIRE(generation != 0);
  const auto message_iterator = std::ranges::find_if(events, [](const auto &event) {
    const auto *message = std::get_if<gisland::ModuleMessageEvent>(&event);
    return message != nullptr && message->instance_id == "owner" &&
           std::holds_alternative<gisland::PublishMessage>(message->message);
  });
  REQUIRE(message_iterator != events.end());
  CHECK(std::get<gisland::ModuleMessageEvent>(*message_iterator).generation == generation);
  const auto &publish = std::get<gisland::PublishMessage>(
      std::get<gisland::ModuleMessageEvent>(*message_iterator).message);
  CHECK(publish.context_id == "fake");

  stop_and_wait(supervisor, events, "owner");
  CHECK(count_events<gisland::ContextsRemovedEvent>(events, "owner") >= 1);
  CHECK(count_events<gisland::ProcessExitedEvent>(events, "owner") == 1);
  CHECK(std::ranges::all_of(events, [generation](const auto &event) {
    if (const auto *removed = std::get_if<gisland::ContextsRemovedEvent>(&event)) {
      return removed->instance_id != "owner" || removed->generation == generation;
    }
    if (const auto *exited = std::get_if<gisland::ProcessExitedEvent>(&event)) {
      return exited->instance_id != "owner" || exited->generation == generation;
    }
    return true;
  }));

  const auto removal = event_index(events, [](const auto &event) {
    const auto *typed = std::get_if<gisland::ContextsRemovedEvent>(&event);
    return typed != nullptr && typed->instance_id == "owner";
  });
  const auto stopped = event_index(events, [](const auto &event) {
    const auto *typed = std::get_if<gisland::StateChangedEvent>(&event);
    return typed != nullptr && typed->instance_id == "owner" &&
           typed->transition.to == gisland::ModuleState::stopped;
  });
  REQUIRE(removal.has_value());
  REQUIRE(stopped.has_value());
  CHECK(removal.value_or(events.size()) < stopped.value_or(events.size()));
}

TEST_CASE("supervisor emits data snapshots only after capability negotiation") {
  SECTION("negotiated data is emitted") {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    auto request = fake_request("data", "data");
    request.init.maximum = {.major = 1, .minor = 1};
    request.init.capabilities.emplace_back("data-snapshots");
    const auto started = supervisor.start(std::move(request));
    REQUIRE(started.has_value());

    collect_until(supervisor, events, [](const auto &observed) {
      return has_message<gisland::DataMessage>(observed, "data");
    });
    stop_and_wait(supervisor, events, "data");
  }

  SECTION("data without capability is a violation") {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    REQUIRE(supervisor.start(fake_request("unnegotiated", "data-without-capability")).has_value());

    collect_until(supervisor, events, [](const auto &observed) {
      return count_events<gisland::ProtocolViolationEvent>(observed, "unnegotiated") > 0;
    });
    CHECK_FALSE(has_message<gisland::DataMessage>(events, "unnegotiated"));
    stop_and_wait(supervisor, events, "unnegotiated");
  }

  SECTION("data before ready fails startup") {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    REQUIRE(supervisor.start(fake_request("early", "data-before-ready")).has_value());

    collect_until(supervisor, events, [](const auto &observed) {
      return has_state(observed, "early", gisland::ModuleState::stopped);
    });
    CHECK(count_events<gisland::ProtocolViolationEvent>(events, "early") > 0);
    CHECK_FALSE(has_message<gisland::DataMessage>(events, "early"));
  }
}

TEST_CASE("supervisor emits image publications only after capability negotiation") {
  SECTION("negotiated images are emitted") {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    auto request = fake_request("image", "image");
    request.init.maximum = {.major = 1, .minor = 2};
    request.init.capabilities.emplace_back("context-images");
    REQUIRE(supervisor.start(std::move(request)).has_value());

    collect_until(supervisor, events, [](const auto &observed) {
      return has_message<gisland::PublishMessage>(observed, "image");
    });
    stop_and_wait(supervisor, events, "image");
  }

  SECTION("images without capability are violations") {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    auto request = fake_request("unnegotiated-image", "image-without-capability");
    request.init.maximum = {.major = 1, .minor = 2};
    request.init.capabilities.emplace_back("context-images");
    REQUIRE(supervisor.start(std::move(request)).has_value());

    collect_until(supervisor, events, [](const auto &observed) {
      return count_events<gisland::ProtocolViolationEvent>(observed, "unnegotiated-image") > 0;
    });
    CHECK_FALSE(has_message<gisland::PublishMessage>(events, "unnegotiated-image"));
    stop_and_wait(supervisor, events, "unnegotiated-image");
  }
}

TEST_CASE("supervisor emits independent views only after protocol 1.4 capability negotiation") {
  SECTION("negotiated independent views are emitted") {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    auto request = fake_request("independent", "independent");
    request.init.maximum = {.major = 1, .minor = 4};
    request.init.capabilities.emplace_back("independent-views");
    REQUIRE(supervisor.start(std::move(request)).has_value());

    collect_until(supervisor, events, [](const auto &observed) {
      return has_message<gisland::PublishMessage>(observed, "independent");
    });
    stop_and_wait(supervisor, events, "independent");
  }

  SECTION("independent views without capability are violations") {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    auto request = fake_request("unnegotiated-independent", "independent-without-capability");
    request.init.maximum = {.major = 1, .minor = 4};
    request.init.capabilities.emplace_back("independent-views");
    REQUIRE(supervisor.start(std::move(request)).has_value());

    collect_until(supervisor, events, [](const auto &observed) {
      return count_events<gisland::ProtocolViolationEvent>(observed, "unnegotiated-independent") >
             0;
    });
    CHECK_FALSE(has_message<gisland::PublishMessage>(events, "unnegotiated-independent"));
    stop_and_wait(supervisor, events, "unnegotiated-independent");
  }
}

TEST_CASE("supervisor gates status indicators on protocol 1.6 capability negotiation") {
  SECTION("negotiated indicators are emitted") {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    auto request = fake_request("indicator", "indicator");
    request.init.maximum = {.major = 1, .minor = 6};
    request.init.capabilities.emplace_back("status-indicator");
    REQUIRE(supervisor.start(std::move(request)).has_value());

    collect_until(supervisor, events, [](const auto &observed) {
      return has_message<gisland::PublishMessage>(observed, "indicator");
    });
    stop_and_wait(supervisor, events, "indicator");
  }

  for (const auto &[instance, mode, maximum, error_path] :
       std::vector<std::tuple<std::string, std::string, gisland::ProtocolVersion, std::string>>{
           {"unnegotiated-indicator", "indicator-without-capability", {1, 6}, "/compact"},
           {"legacy-indicator", "indicator-legacy", {1, 5}, "/compact"},
           {"legacy-expanded-indicator", "indicator-expanded-legacy", {1, 5}, "/expanded"},
           {"early-indicator-capability",
            "indicator-capability-on-1.5",
            {1, 6},
            "/capabilities"}}) {
    SECTION(instance) {
      gisland::ModuleSupervisor supervisor;
      EventLog events;
      auto request = fake_request(instance, mode);
      request.init.maximum = maximum;
      request.init.capabilities.emplace_back("status-indicator");
      REQUIRE(supervisor.start(std::move(request)).has_value());

      collect_until(supervisor, events, [&instance](const auto &observed) {
        return count_events<gisland::ProtocolViolationEvent>(observed, instance) > 0;
      });
      CHECK_FALSE(has_message<gisland::PublishMessage>(events, instance));
      REQUIRE(last_violation(events, instance) != nullptr);
      CHECK(last_violation(events, instance)->error.path == error_path);
      stop_and_wait(supervisor, events, instance);
    }
  }
}

TEST_CASE("supervisor gates audio HUD fields on protocol 1.7 capabilities") {
  SECTION("negotiated HUD fields are emitted") {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    auto request = fake_request("audio-hud", "audio-hud");
    request.init.maximum = {.major = 1, .minor = 7};
    request.init.capabilities.insert(
        request.init.capabilities.end(),
        {"independent-views", "compact-view-styles", "icon-roles", "progress-transitions"});
    REQUIRE(supervisor.start(std::move(request)).has_value());

    collect_until(supervisor, events, [](const auto &observed) {
      return has_message<gisland::PublishMessage>(observed, "audio-hud");
    });
    stop_and_wait(supervisor, events, "audio-hud");
  }

  for (const auto &[instance, mode, capability, error_path] :
       std::vector<std::tuple<std::string, std::string, std::string, std::string>>{
           {"hud-style-missing", "hud-style-without-capability", "compact-view-styles",
            "/presentation/compact_style"},
           {"icon-role-missing", "icon-role-without-capability", "icon-roles", "/views/compact"},
           {"progress-transition-missing", "progress-transition-without-capability",
            "progress-transitions", "/views/compact"},
           {"early-hud-capability", "hud-capability-on-1.6", "compact-view-styles",
            "/capabilities"}}) {
    SECTION(instance) {
      gisland::ModuleSupervisor supervisor;
      EventLog events;
      auto request = fake_request(instance, mode);
      request.init.maximum = {.major = 1, .minor = 7};
      request.init.capabilities.emplace_back("independent-views");
      request.init.capabilities.emplace_back(capability);
      REQUIRE(supervisor.start(std::move(request)).has_value());

      collect_until(supervisor, events, [&instance](const auto &observed) {
        return count_events<gisland::ProtocolViolationEvent>(observed, instance) > 0;
      });
      CHECK_FALSE(has_message<gisland::PublishMessage>(events, instance));
      REQUIRE(last_violation(events, instance) != nullptr);
      CHECK(last_violation(events, instance)->error.path == error_path);
      stop_and_wait(supervisor, events, instance);
    }
  }
}

TEST_CASE("supervisor exchanges typed actions visibility and tagged stderr") {
  gisland::ModuleSupervisor supervisor;
  EventLog events;
  REQUIRE(supervisor.start(fake_request("interactive", "stderr")).has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return has_state(observed, "interactive", gisland::ModuleState::running) &&
           count_events<gisland::StderrLogEvent>(observed, "interactive") >= 2;
  });

  std::vector<std::string> stderr_lines;
  for (const auto &event : events) {
    if (const auto *log = std::get_if<gisland::StderrLogEvent>(&event);
        log != nullptr && log->instance_id == "interactive") {
      stderr_lines.push_back(log->line);
      CHECK_FALSE(log->truncated);
    }
  }
  CHECK((stderr_lines == std::vector<std::string>{"fake diagnostic one", "fake diagnostic two"}));

  REQUIRE(supervisor
              .send("interactive", gisland::CoreMessage{gisland::ActionMessage{
                                       .action_id = "calendar.today",
                                       .value = std::nullopt,
                                   }})
              .has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return has_message<gisland::ActionResultMessage>(observed, "interactive");
  });

  REQUIRE(supervisor
              .send("interactive", gisland::CoreMessage{gisland::VisibilityMessage{
                                       gisland::Visibility::expanded_active}})
              .has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return has_message<gisland::LogMessage>(observed, "interactive");
  });

  stop_and_wait(supervisor, events, "interactive");
}

TEST_CASE("invocation-aware sends report target-generation queue admission exactly once") {
  gisland::ModuleSupervisor supervisor;
  EventLog events;
  REQUIRE(supervisor.start(fake_request("correlated", "ready")).has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return has_state(observed, "correlated", gisland::ModuleState::running);
  });
  const auto generation = process_generation(events, "correlated");
  REQUIRE(generation != 0);

  REQUIRE(supervisor
              .send_action("correlated", generation,
                           gisland::ActionMessage{
                               .action_id = "calendar.today",
                               .value = std::nullopt,
                               .invocation_id = 101,
                           })
              .has_value());
  collect_until(supervisor, events,
                [](const auto &observed) { return delivery(observed, 101) != nullptr; });

  REQUIRE(delivery(events, 101) != nullptr);
  CHECK(delivery(events, 101)->instance_id == "correlated");
  CHECK(delivery(events, 101)->generation == generation);
  CHECK(delivery(events, 101)->succeeded);
  CHECK_FALSE(delivery(events, 101)->error.has_value());
  CHECK(count_events<gisland::ActionDeliveryEvent>(events, "correlated") == 1);
  stop_and_wait(supervisor, events, "correlated");
}

TEST_CASE("invocation-aware sends reject unavailable targets and stale generations") {
  SECTION("unknown") {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    REQUIRE(supervisor
                .send_action("missing", 1,
                             gisland::ActionMessage{
                                 .action_id = "probe", .value = std::nullopt, .invocation_id = 201})
                .has_value());
    collect_until(supervisor, events,
                  [](const auto &observed) { return delivery(observed, 201) != nullptr; });
    CHECK(delivery(events, 201)->error == gisland::ActionDeliveryError::unknown_instance);
  }

  SECTION("starting") {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    REQUIRE(supervisor.start(fake_request("starting", "silent")).has_value());
    REQUIRE(supervisor
                .send_action("starting", 1,
                             gisland::ActionMessage{
                                 .action_id = "probe", .value = std::nullopt, .invocation_id = 202})
                .has_value());
    collect_until(supervisor, events,
                  [](const auto &observed) { return delivery(observed, 202) != nullptr; });
    CHECK(delivery(events, 202)->error == gisland::ActionDeliveryError::unavailable);
    supervisor.shutdown();
  }

  SECTION("stopping") {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    REQUIRE(supervisor.start(fake_request("stopping", "ready")).has_value());
    collect_until(supervisor, events, [](const auto &observed) {
      return has_state(observed, "stopping", gisland::ModuleState::running);
    });
    const auto generation = process_generation(events, "stopping");
    REQUIRE(supervisor.stop("stopping").has_value());
    REQUIRE(supervisor
                .send_action("stopping", generation,
                             gisland::ActionMessage{
                                 .action_id = "probe", .value = std::nullopt, .invocation_id = 203})
                .has_value());
    collect_until(supervisor, events,
                  [](const auto &observed) { return delivery(observed, 203) != nullptr; });
    CHECK(delivery(events, 203)->error == gisland::ActionDeliveryError::unavailable);
    supervisor.shutdown();
  }

  SECTION("exited") {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    REQUIRE(supervisor.start(fake_request("exited", "exit-zero")).has_value());
    collect_until(supervisor, events, [](const auto &observed) {
      return has_state(observed, "exited", gisland::ModuleState::stopped);
    });
    const auto generation = process_generation(events, "exited");
    REQUIRE(supervisor
                .send_action("exited", generation,
                             gisland::ActionMessage{
                                 .action_id = "probe", .value = std::nullopt, .invocation_id = 204})
                .has_value());
    collect_until(supervisor, events,
                  [](const auto &observed) { return delivery(observed, 204) != nullptr; });
    CHECK(delivery(events, 204)->error == gisland::ActionDeliveryError::unavailable);
  }

  SECTION("replaced") {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    REQUIRE(supervisor.start(fake_request("replaced", "ready")).has_value());
    collect_until(supervisor, events, [](const auto &observed) {
      return has_state(observed, "replaced", gisland::ModuleState::running);
    });
    const auto old_generation = process_generation(events, "replaced");
    REQUIRE(supervisor
                .reconfigure(gisland::SupervisorReconfiguration{
                    .stop_instances = {},
                    .start_or_replace = {fake_request("replaced", "ready")},
                })
                .has_value());
    collect_until(supervisor, events, [](const auto &observed) {
      return count_events<gisland::ProcessStartedEvent>(observed, "replaced") == 2 &&
             has_state(observed, "replaced", gisland::ModuleState::running);
    });
    REQUIRE(supervisor
                .send_action("replaced", old_generation,
                             gisland::ActionMessage{
                                 .action_id = "probe", .value = std::nullopt, .invocation_id = 205})
                .has_value());
    collect_until(supervisor, events,
                  [](const auto &observed) { return delivery(observed, 205) != nullptr; });
    CHECK(delivery(events, 205)->error == gisland::ActionDeliveryError::generation_mismatch);
    stop_and_wait(supervisor, events, "replaced");
  }
}

TEST_CASE("invocation-aware send reports write-queue rejection without false delivery") {
  gisland::ModuleSupervisor supervisor;
  EventLog events;
  REQUIRE(supervisor.start(fake_request("saturated", "ready-ignore-stdin")).has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return has_state(observed, "saturated", gisland::ModuleState::running);
  });
  const auto generation = process_generation(events, "saturated");
  const nlohmann::json oversized_value = std::string((std::size_t{1024} * 1024U) + 1U, 'x');
  REQUIRE(supervisor
              .send_action("saturated", generation,
                           gisland::ActionMessage{.action_id = "oversized",
                                                  .value = oversized_value,
                                                  .invocation_id = 301})
              .has_value());
  collect_until(supervisor, events,
                [](const auto &observed) { return delivery(observed, 301) != nullptr; });
  REQUIRE(delivery(events, 301) != nullptr);
  CHECK_FALSE(delivery(events, 301)->succeeded);
  CHECK(delivery(events, 301)->error == gisland::ActionDeliveryError::queue_saturated);
  CHECK(count_events<gisland::ActionDeliveryEvent>(events, "saturated") == 1);
  supervisor.shutdown();
}

TEST_CASE("queued old-generation message and exit remain bound across replacement") {
  gisland::ModuleSupervisor supervisor;
  EventLog events;
  REQUIRE(supervisor.start(fake_request("queued-replacement", "publish")).has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return has_message<gisland::PublishMessage>(observed, "queued-replacement");
  });
  const auto old_generation = process_generation(events, "queued-replacement");
  REQUIRE(old_generation != 0);

  REQUIRE(supervisor
              .reconfigure(gisland::SupervisorReconfiguration{
                  .stop_instances = {},
                  .start_or_replace = {fake_request("queued-replacement", "ready")},
              })
              .has_value());
  collect_until(supervisor, events, [old_generation](const auto &observed) {
    return count_events<gisland::ProcessStartedEvent>(observed, "queued-replacement") == 2 &&
           std::ranges::any_of(observed, [old_generation](const auto &event) {
             const auto *message = std::get_if<gisland::ModuleMessageEvent>(&event);
             return message != nullptr && message->instance_id == "queued-replacement" &&
                    message->generation > old_generation;
           });
  });
  const auto new_generation = process_generation(events, "queued-replacement", 1);
  REQUIRE(new_generation > old_generation);

  const auto old_message = std::ranges::find_if(events, [old_generation](const auto &event) {
    const auto *message = std::get_if<gisland::ModuleMessageEvent>(&event);
    return message != nullptr && message->instance_id == "queued-replacement" &&
           message->generation == old_generation &&
           std::holds_alternative<gisland::PublishMessage>(message->message);
  });
  const auto old_exit = std::ranges::find_if(events, [old_generation](const auto &event) {
    const auto *exited = std::get_if<gisland::ProcessExitedEvent>(&event);
    return exited != nullptr && exited->instance_id == "queued-replacement" &&
           exited->generation == old_generation;
  });
  const auto new_message = std::ranges::find_if(events, [new_generation](const auto &event) {
    const auto *message = std::get_if<gisland::ModuleMessageEvent>(&event);
    return message != nullptr && message->instance_id == "queued-replacement" &&
           message->generation == new_generation;
  });
  REQUIRE(old_message != events.end());
  REQUIRE(old_exit != events.end());
  REQUIRE(new_message != events.end());
  CHECK(std::ranges::none_of(events, [new_generation](const auto &event) {
    const auto *exited = std::get_if<gisland::ProcessExitedEvent>(&event);
    return exited != nullptr && exited->instance_id == "queued-replacement" &&
           exited->generation == new_generation;
  }));
  stop_and_wait(supervisor, events, "queued-replacement");
}

TEST_CASE("supervisor binds dismiss messages to the core-owned instance") {
  gisland::ModuleSupervisor supervisor;
  EventLog events;
  REQUIRE(supervisor.start(fake_request("dismiss-owner", "dismiss")).has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return has_message<gisland::DismissMessage>(observed, "dismiss-owner");
  });

  const auto dismissed = std::ranges::find_if(events, [](const auto &event) {
    const auto *message = std::get_if<gisland::ModuleMessageEvent>(&event);
    return message != nullptr && message->instance_id == "dismiss-owner" &&
           std::holds_alternative<gisland::DismissMessage>(message->message);
  });
  REQUIRE(dismissed != events.end());
  CHECK(std::get<gisland::DismissMessage>(std::get<gisland::ModuleMessageEvent>(*dismissed).message)
            .context_id == "retired");

  stop_and_wait(supervisor, events, "dismiss-owner");
}

TEST_CASE("handshake timeout escalates and is a logs-only module failure") {
  auto timings = fast_timings();
  timings.handshake = 50ms;
  timings.graceful_shutdown = 20ms;
  timings.terminate_grace = 20ms;
  gisland::ModuleSupervisor supervisor;
  EventLog events;
  REQUIRE(supervisor.start(fake_request("silent", "silent", gisland::RestartPolicy::never, timings))
              .has_value());

  collect_until(supervisor, events, [](const auto &observed) {
    return has_state_cause(observed, "silent", gisland::ModuleState::stopped,
                           gisland::StopCause::handshake_timeout);
  });

  CHECK(count_events<gisland::ProtocolViolationEvent>(events, "silent") == 0);
  const auto exited = std::ranges::find_if(events, [](const auto &event) {
    const auto *typed = std::get_if<gisland::ProcessExitedEvent>(&event);
    return typed != nullptr && typed->instance_id == "silent";
  });
  REQUIRE(exited != events.end());
  CHECK(std::get<gisland::ProcessExitedEvent>(*exited).cause ==
        gisland::StopCause::handshake_timeout);
}

TEST_CASE("invalid readiness never destabilizes the supervisor") {
  for (const std::string mode :
       {"malformed", "business-before-ready", "incompatible-ready", "unsupported-capability"}) {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    const std::string instance = "invalid-" + mode;
    REQUIRE(supervisor.start(fake_request(instance, mode)).has_value());
    collect_until(supervisor, events, [&instance](const auto &observed) {
      return has_state_cause(observed, instance, gisland::ModuleState::stopped,
                             gisland::StopCause::protocol_violation);
    });
    CHECK(count_events<gisland::ProtocolViolationEvent>(events, instance) >= 1);
  }
}

TEST_CASE("protocol violations enforce consecutive and rolling thresholds") {
  SECTION("three consecutive") {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    REQUIRE(supervisor.start(fake_request("duplicates", "duplicate-ready")).has_value());
    collect_until(supervisor, events, [](const auto &observed) {
      return has_state_cause(observed, "duplicates", gisland::ModuleState::stopped,
                             gisland::StopCause::protocol_violation);
    });
    CHECK(has_state(events, "duplicates", gisland::ModuleState::running));
    std::size_t maximum_consecutive = 0;
    for (const auto &event : events) {
      if (const auto *violation = std::get_if<gisland::ProtocolViolationEvent>(&event);
          violation != nullptr && violation->instance_id == "duplicates") {
        maximum_consecutive = std::max(maximum_consecutive, violation->consecutive);
      }
    }
    CHECK(maximum_consecutive == 3);
  }

  SECTION("ten rolling with valid messages between") {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    REQUIRE(supervisor.start(fake_request("rolling", "rolling-violations")).has_value());
    collect_until(supervisor, events, [](const auto &observed) {
      return has_state_cause(observed, "rolling", gisland::ModuleState::stopped,
                             gisland::StopCause::protocol_violation);
    });
    std::size_t maximum_rolling = 0;
    std::size_t maximum_consecutive = 0;
    for (const auto &event : events) {
      if (const auto *violation = std::get_if<gisland::ProtocolViolationEvent>(&event);
          violation != nullptr && violation->instance_id == "rolling") {
        maximum_rolling = std::max(maximum_rolling, violation->rolling);
        maximum_consecutive = std::max(maximum_consecutive, violation->consecutive);
      }
    }
    CHECK(maximum_rolling == 10);
    CHECK(maximum_consecutive == 1);
  }
}

TEST_CASE("final unterminated protocol lines and truncated stderr are retained") {
  SECTION("final protocol line") {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    REQUIRE(supervisor.start(fake_request("final", "final-line")).has_value());
    collect_until(supervisor, events, [](const auto &observed) {
      return has_message<gisland::PublishMessage>(observed, "final") &&
             has_state(observed, "final", gisland::ModuleState::stopped);
    });
  }

  SECTION("stderr truncation") {
    gisland::ModuleSupervisor supervisor;
    EventLog events;
    REQUIRE(supervisor.start(fake_request("long-log", "stderr-long")).has_value());
    collect_until(supervisor, events, [](const auto &observed) {
      return std::ranges::any_of(observed, [](const auto &event) {
        const auto *log = std::get_if<gisland::StderrLogEvent>(&event);
        return log != nullptr && log->instance_id == "long-log" && log->truncated;
      });
    });
    const auto log = std::ranges::find_if(events, [](const auto &event) {
      const auto *typed = std::get_if<gisland::StderrLogEvent>(&event);
      return typed != nullptr && typed->instance_id == "long-log" && typed->truncated;
    });
    REQUIRE(log != events.end());
    CHECK(std::get<gisland::StderrLogEvent>(*log).line.size() == std::size_t{64} * 1024U);
    stop_and_wait(supervisor, events, "long-log");
  }
}

TEST_CASE("owned contexts are removed before crash backoff and restart") {
  const std::string config_text = std::string{R"(
monitor = "primary"
theme = "organic"
default_module = "fake"
[[modules]]
id = "fake"
command = [")"} + GISLAND_FAKE_MODULE_PATH +
                                  R"(", "publish-crash"]
restart = "on-failure"
[modules.timings]
initial_backoff_ms = 20
maximum_backoff_ms = 20
)";
  const auto config = gisland::parse_config(config_text, "integration.toml");
  REQUIRE(config.has_value());
  const auto &module = config->modules.front();
  auto request = fake_request(module.id, "publish-crash", module.restart, module.timings);
  request.process.argv = module.command;

  gisland::ModuleSupervisor supervisor;
  EventLog events;
  const auto started = supervisor.start(std::move(request));
  REQUIRE(started.has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return count_events<gisland::ProcessStartedEvent>(observed, "fake") >= 2 &&
           has_message<gisland::PublishMessage>(observed, "fake") &&
           count_events<gisland::ContextsRemovedEvent>(observed, "fake") >= 1 &&
           has_state(observed, "fake", gisland::ModuleState::backoff);
  });

  gisland::ContextArbiter arbiter{{"clock", "default"}};
  arbiter.publish(
      gisland::PublishedContext{
          .key = {"clock", "default"},
          .priority = 0,
          .expires_at = std::nullopt,
          .compact = gisland::SceneNode{gisland::Text{"default", "body"}},
          .expanded = std::nullopt,
      },
      gisland::MonotonicTime{});
  bool saw_owned_active = false;
  bool saw_removal_after_active = false;
  for (const auto &event : events) {
    if (const auto *message = std::get_if<gisland::ModuleMessageEvent>(&event);
        message != nullptr && message->instance_id == "fake") {
      if (const auto *publish = std::get_if<gisland::PublishMessage>(&message->message)) {
        arbiter.publish(
            gisland::PublishedContext{
                .key = {message->instance_id, publish->context_id},
                .priority = publish->priority,
                .expires_at = publish->expires_in.has_value()
                                  ? std::optional{message->at + *publish->expires_in}
                                  : std::nullopt,
                .compact = publish->compact,
                .expanded = publish->expanded,
            },
            message->at);
        const auto *active = arbiter.active(message->at);
        saw_owned_active =
            saw_owned_active || (active != nullptr && active->key.instance_id == "fake");
      }
    }
    if (const auto *removed = std::get_if<gisland::ContextsRemovedEvent>(&event);
        removed != nullptr && removed->instance_id == "fake") {
      arbiter.dismiss_instance(removed->instance_id);
      const auto *active = arbiter.active(removed->at);
      saw_removal_after_active =
          saw_removal_after_active ||
          (saw_owned_active && active != nullptr && active->key.instance_id != "fake");
    }
  }
  CHECK(saw_owned_active);
  CHECK(saw_removal_after_active);

  stop_and_wait(supervisor, events, "fake");
}

TEST_CASE("failure lockout and manual restart are supervised asynchronously") {
  auto timings = fast_timings();
  timings.initial_backoff = 1ms;
  timings.maximum_backoff = 1ms;
  gisland::ModuleSupervisor supervisor;
  EventLog events;
  REQUIRE(supervisor
              .start(fake_request("failing", "exit-nonzero", gisland::RestartPolicy::on_failure,
                                  timings))
              .has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return has_state(observed, "failing", gisland::ModuleState::failed);
  });
  CHECK(count_events<gisland::ProcessStartedEvent>(events, "failing") == 10);

  REQUIRE(supervisor.restart("failing", 41).has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return count_events<gisland::ProcessStartedEvent>(observed, "failing") >= 11 &&
           std::ranges::any_of(observed, [](const auto &event) {
             const auto *state = std::get_if<gisland::StateChangedEvent>(&event);
             return state != nullptr && state->instance_id == "failing" &&
                    state->transition.from == gisland::ModuleState::failed &&
                    state->transition.to == gisland::ModuleState::starting;
           });
  });
  collect_until(supervisor, events, [](const auto &observed) {
    return std::ranges::any_of(observed, [](const auto &event) {
      const auto *completed = std::get_if<gisland::RestartCompletedEvent>(&event);
      return completed != nullptr && completed->instance_id == "failing" &&
             completed->generation == 41 && !completed->succeeded;
    });
  });
  stop_and_wait(supervisor, events, "failing");
}

TEST_CASE("explicit restart completion is correlated with its generation") {
  gisland::ModuleSupervisor supervisor;
  EventLog events;
  REQUIRE(supervisor.start(fake_request("ready", "ready")).has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return has_state(observed, "ready", gisland::ModuleState::running);
  });

  REQUIRE(supervisor.restart("ready", 77).has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return std::ranges::any_of(observed, [](const auto &event) {
      const auto *completed = std::get_if<gisland::RestartCompletedEvent>(&event);
      return completed != nullptr && completed->instance_id == "ready" &&
             completed->generation == 77 && completed->succeeded &&
             completed->state == gisland::ModuleState::running;
    });
  });
  CHECK(std::ranges::none_of(events, [](const auto &event) {
    const auto *completed = std::get_if<gisland::RestartCompletedEvent>(&event);
    return completed != nullptr && completed->generation != 77;
  }));
  stop_and_wait(supervisor, events, "ready");
}

TEST_CASE(
    "grouped supervisor reconfiguration preserves unchanged processes and replaces in order") {
  gisland::ModuleSupervisor supervisor;
  EventLog events;
  REQUIRE(supervisor.start(fake_request("stable", "ready")).has_value());
  REQUIRE(supervisor.start(fake_request("changed", "ready")).has_value());
  REQUIRE(supervisor.start(fake_request("removed", "ready")).has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return has_state(observed, "stable", gisland::ModuleState::running) &&
           has_state(observed, "changed", gisland::ModuleState::running) &&
           has_state(observed, "removed", gisland::ModuleState::running);
  });
  const auto stable_started = event_index(events, [](const auto &event) {
    const auto *started = std::get_if<gisland::ProcessStartedEvent>(&event);
    return started != nullptr && started->instance_id == "stable";
  });
  REQUIRE(stable_started.has_value());
  const std::size_t stable_started_index = stable_started.value_or(events.size());
  const pid_t stable_pid = std::get<gisland::ProcessStartedEvent>(events[stable_started_index]).pid;

  REQUIRE(supervisor
              .reconfigure(gisland::SupervisorReconfiguration{
                  .stop_instances = {"removed"},
                  .start_or_replace = {fake_request("changed", "ready"),
                                       fake_request("added", "ready")},
              })
              .has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return count_events<gisland::ProcessStartedEvent>(observed, "changed") == 2 &&
           has_state(observed, "added", gisland::ModuleState::running) &&
           has_state(observed, "removed", gisland::ModuleState::stopped);
  });

  CHECK(count_events<gisland::ProcessStartedEvent>(events, "stable") == 1);
  CHECK(std::get<gisland::ProcessStartedEvent>(events[stable_started_index]).pid == stable_pid);
  CHECK(count_events<gisland::ContextsRemovedEvent>(events, "stable") == 0);
  const auto changed_exit = event_index(events, [](const auto &event) {
    const auto *exited = std::get_if<gisland::ProcessExitedEvent>(&event);
    return exited != nullptr && exited->instance_id == "changed";
  });
  const auto changed_replacement = event_index(events, [](const auto &event) {
    const auto *started = std::get_if<gisland::ProcessStartedEvent>(&event);
    return started != nullptr && started->instance_id == "changed";
  });
  REQUIRE(changed_exit.has_value());
  REQUIRE(changed_replacement.has_value());
  const std::size_t changed_replacement_index = changed_replacement.value_or(events.size());
  const auto second_changed_start = std::ranges::find_if(
      events.begin() + static_cast<std::ptrdiff_t>(changed_replacement_index + 1), events.end(),
      [](const auto &event) {
        const auto *started = std::get_if<gisland::ProcessStartedEvent>(&event);
        return started != nullptr && started->instance_id == "changed";
      });
  REQUIRE(second_changed_start != events.end());
  CHECK(changed_exit.value_or(events.size()) <
        static_cast<std::size_t>(std::distance(events.begin(), second_changed_start)));
  supervisor.shutdown();
}

TEST_CASE("grouped supervisor reconfiguration cancels an affected explicit restart") {
  gisland::ModuleSupervisor supervisor;
  EventLog events;
  REQUIRE(supervisor.start(fake_request("changed", "ready")).has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return has_state(observed, "changed", gisland::ModuleState::running);
  });

  REQUIRE(supervisor.restart("changed", 91).has_value());
  REQUIRE(supervisor
              .reconfigure(gisland::SupervisorReconfiguration{
                  .stop_instances = {},
                  .start_or_replace = {fake_request("changed", "ready")},
              })
              .has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return std::ranges::any_of(observed,
                               [](const auto &event) {
                                 const auto *completed =
                                     std::get_if<gisland::RestartCompletedEvent>(&event);
                                 return completed != nullptr &&
                                        completed->instance_id == "changed" &&
                                        completed->generation == 91 && !completed->succeeded;
                               }) &&
           count_events<gisland::ProcessStartedEvent>(observed, "changed") == 2 &&
           has_state(observed, "changed", gisland::ModuleState::running);
  });
  supervisor.shutdown();
}

TEST_CASE("outbound saturation terminates an unresponsive module without blocking caller") {
  gisland::ModuleSupervisor supervisor;
  EventLog events;
  REQUIRE(supervisor.start(fake_request("blocked", "ready-ignore-stdin")).has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return has_state(observed, "blocked", gisland::ModuleState::running);
  });

  const nlohmann::json oversized_value = std::string((std::size_t{1024} * 1024U) + 1U, 'x');
  REQUIRE(supervisor
              .send("blocked", gisland::CoreMessage{gisland::ActionMessage{
                                   .action_id = "oversized",
                                   .value = oversized_value,
                               }})
              .has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return has_state_cause(observed, "blocked", gisland::ModuleState::stopped,
                           gisland::StopCause::unresponsive);
  });
}

TEST_CASE("closed child stdin is an isolated module I/O failure") {
  gisland::ModuleSupervisor supervisor;
  EventLog events;
  REQUIRE(supervisor.start(fake_request("refused", "refuse-stdin")).has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return has_state(observed, "refused", gisland::ModuleState::running);
  });

  REQUIRE(supervisor
              .send("refused", gisland::CoreMessage{gisland::ActionMessage{
                                   .action_id = "probe",
                                   .value = std::nullopt,
                               }})
              .has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return has_state_cause(observed, "refused", gisland::ModuleState::stopped,
                           gisland::StopCause::io_error);
  });
}

TEST_CASE("ignored shutdown escalates through SIGTERM to SIGKILL and never restarts") {
  auto timings = fast_timings();
  timings.graceful_shutdown = 20ms;
  timings.terminate_grace = 20ms;
  gisland::ModuleSupervisor supervisor;
  EventLog events;
  REQUIRE(supervisor
              .start(fake_request("stubborn", "ignore-shutdown", gisland::RestartPolicy::always,
                                  timings))
              .has_value());
  collect_until(supervisor, events, [](const auto &observed) {
    return has_state(observed, "stubborn", gisland::ModuleState::running);
  });

  stop_and_wait(supervisor, events, "stubborn");
  const auto exit = std::ranges::find_if(events, [](const auto &event) {
    const auto *typed = std::get_if<gisland::ProcessExitedEvent>(&event);
    return typed != nullptr && typed->instance_id == "stubborn";
  });
  REQUIRE(exit != events.end());
  const auto &status = std::get<gisland::ProcessExitedEvent>(*exit).status;
  CHECK(status.kind == gisland::ExitKind::signaled);
  CHECK(status.code == SIGKILL);
  CHECK(count_events<gisland::ProcessStartedEvent>(events, "stubborn") == 1);
}

TEST_CASE(
    "thirty_two_modules start and shut down concurrently without zombies or descriptor leaks") {
  const auto descriptors_before = open_descriptor_count();
  EventLog events;
  std::vector<pid_t> child_pids;
  {
    gisland::ModuleSupervisor supervisor;
    const auto enqueue_started = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < gisland::ModuleSupervisor::maximum_instances; ++index) {
      REQUIRE(
          supervisor.start(fake_request("module-" + std::to_string(index), "ready")).has_value());
    }
    CHECK(std::chrono::steady_clock::now() - enqueue_started < 100ms);

    collect_until(
        supervisor, events,
        [](const auto &observed) {
          std::size_t running = 0;
          for (std::size_t index = 0; index < gisland::ModuleSupervisor::maximum_instances;
               ++index) {
            running += has_state(observed, "module-" + std::to_string(index),
                                 gisland::ModuleState::running)
                           ? 1U
                           : 0U;
          }
          return running == gisland::ModuleSupervisor::maximum_instances;
        },
        5s);

    REQUIRE(supervisor.start(fake_request("overflow", "ready")).has_value());
    collect_until(supervisor, events, [](const auto &observed) {
      return std::ranges::any_of(observed, [](const auto &event) {
        const auto *error = std::get_if<gisland::SupervisorErrorEvent>(&event);
        return error != nullptr && error->instance_id == "overflow" &&
               error->message.contains("32-instance");
      });
    });
    CHECK(count_events<gisland::ProcessStartedEvent>(events, "overflow") == 0);

    for (const auto &event : events) {
      if (const auto *started = std::get_if<gisland::ProcessStartedEvent>(&event);
          started != nullptr && started->instance_id.starts_with("module-")) {
        child_pids.push_back(started->pid);
      }
    }
    REQUIRE(child_pids.size() == gisland::ModuleSupervisor::maximum_instances);

    supervisor.shutdown();
    auto final_events = supervisor.drain_events();
    events.insert(events.end(), std::make_move_iterator(final_events.begin()),
                  std::make_move_iterator(final_events.end()));

    for (std::size_t index = 0; index < gisland::ModuleSupervisor::maximum_instances; ++index) {
      const auto instance_id = "module-" + std::to_string(index);
      CHECK(has_state(events, instance_id, gisland::ModuleState::stopped));
      CHECK(count_events<gisland::ProcessExitedEvent>(events, instance_id) == 1);
    }
  }

  CHECK(open_descriptor_count() == descriptors_before);
  for (const pid_t pid : child_pids) {
    errno = 0;
    CHECK(::waitpid(pid, nullptr, WNOHANG) == -1);
    CHECK(errno == ECHILD);
  }
}

TEST_CASE("grouped reconfiguration defers additions until stopping capacity is released") {
  gisland::ModuleSupervisor supervisor;
  EventLog events;
  for (std::size_t index = 0; index < gisland::ModuleSupervisor::maximum_instances; ++index) {
    REQUIRE(
        supervisor.start(fake_request("capacity-" + std::to_string(index), "ready")).has_value());
  }
  collect_until(
      supervisor, events,
      [](const auto &observed) {
        return count_events<gisland::ProcessStartedEvent>(observed, "capacity-31") == 1;
      },
      5s);

  REQUIRE(supervisor
              .reconfigure(gisland::SupervisorReconfiguration{
                  .stop_instances = {"capacity-0"},
                  .start_or_replace = {fake_request("capacity-added", "ready")},
              })
              .has_value());
  collect_until(
      supervisor, events,
      [](const auto &observed) {
        return has_state(observed, "capacity-0", gisland::ModuleState::stopped) &&
               has_state(observed, "capacity-added", gisland::ModuleState::running);
      },
      5s);
  CHECK(count_events<gisland::ProcessStartedEvent>(events, "capacity-added") == 1);
  supervisor.shutdown();
}
