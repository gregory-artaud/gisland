#include "gisland/config.hpp"
#include "gisland/process_backend.hpp"
#include "gisland/protocol.hpp"
#include "gisland/scene_template.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef GISLAND_CLOCK_CALENDAR_PATH
#error "GISLAND_CLOCK_CALENDAR_PATH must name the clock-calendar module"
#endif
#ifndef GISLAND_TEST_ASSET_ROOT
#error "GISLAND_TEST_ASSET_ROOT must name the copied distributed assets"
#endif

namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::span<const std::byte> bytes(std::string_view value) {
  return std::as_bytes(std::span{value.data(), value.size()});
}

class ModuleProcess {
public:
  ModuleProcess() : process_(spawn(backend_)) {}

  ModuleProcess(const ModuleProcess &) = delete;
  ModuleProcess &operator=(const ModuleProcess &) = delete;

  ~ModuleProcess() {
    if (exited_) {
      return;
    }
    const auto status = backend_.reap(process_);
    if (status && !status->has_value()) {
      const auto signaled = backend_.signal_group(process_, SIGKILL);
      if (!signaled) {
        return;
      }
      for (int attempt = 0; attempt < 100; ++attempt) {
        const auto reaped = backend_.reap(process_);
        if (reaped && reaped->has_value()) {
          break;
        }
        if (!backend_.poll({}, 1ms)) {
          break;
        }
      }
    }
  }

  void send(std::string_view value) {
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (offset < value.size() && std::chrono::steady_clock::now() < deadline) {
      const auto written = backend_.write_stdin(process_, bytes(value.substr(offset)));
      REQUIRE(written.has_value());
      offset += written->transferred;
      if (written->would_block) {
        const std::array interests{
            gisland::PollInterest{process_.stdin_fd(), false, true, 1},
        };
        REQUIRE(backend_.poll(interests, 25ms).has_value());
      }
    }
    REQUIRE(offset == value.size());
  }

  [[nodiscard]] nlohmann::json read_json() {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (const auto newline = stdout_.find('\n'); newline != std::string::npos) {
        const std::string line = stdout_.substr(0, newline);
        stdout_.erase(0, newline + 1);
        return nlohmann::json::parse(line);
      }
      std::array<std::byte, 4096> buffer{};
      const auto read = backend_.read_stdout(process_, buffer);
      REQUIRE(read.has_value());
      if (read->transferred > 0) {
        stdout_.append(reinterpret_cast<const char *>(buffer.data()), read->transferred);
        continue;
      }
      if (read->eof) {
        break;
      }
      const std::array interests{
          gisland::PollInterest{process_.stdout_fd(), true, false, 2},
      };
      REQUIRE(backend_.poll(interests, 25ms).has_value());
    }
    FAIL("timed out waiting for module output");
    return {};
  }

  [[nodiscard]] bool output_ready(std::chrono::milliseconds timeout) {
    if (stdout_.contains('\n')) {
      return true;
    }
    const std::array interests{
        gisland::PollInterest{process_.stdout_fd(), true, false, 3},
    };
    const auto ready = backend_.poll(interests, timeout);
    REQUIRE(ready.has_value());
    return !ready->empty();
  }

  [[nodiscard]] gisland::ExitStatus wait_for_exit() {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto status = backend_.reap(process_);
      REQUIRE(status.has_value());
      if (status->has_value()) {
        const auto value = status->value();
        exited_ = true;
        return value;
      }
      REQUIRE(backend_.poll({}, 10ms).has_value());
    }
    FAIL("timed out waiting for module exit");
    return {gisland::ExitKind::exited, -1};
  }

private:
  [[nodiscard]] static gisland::ProcessHandle spawn(const gisland::ProcessBackend &backend) {
    auto spawned = backend.spawn({
        .argv = {GISLAND_CLOCK_CALENDAR_PATH},
        .environment = {},
        .working_directory = std::nullopt,
    });
    if (!spawned) {
      throw std::runtime_error("failed to spawn clock-calendar: " + spawned.error().message);
    }
    return std::move(spawned).value();
  }

  gisland::ProcessBackend backend_;
  gisland::ProcessHandle process_;
  std::string stdout_;
  bool exited_{false};
};

[[nodiscard]] gisland::InitMessage init(nlohmann::json configuration = nlohmann::json::object()) {
  return {
      .minimum = {1, 0},
      .maximum = {1, 1},
      .instance_id = "clock",
      .capabilities = {"data-snapshots"},
      .configuration = std::move(configuration),
      .locale = "C",
      .timezone = "UTC",
  };
}

} // namespace

TEST_CASE("clock-calendar process negotiates and publishes a live snapshot") {
  ModuleProcess process;
  process.send(gisland::serialize_core_message(init()));

  const auto ready = process.read_json();
  CHECK(ready.at("type") == "ready");
  CHECK(ready.at("protocol_major") == 1);
  CHECK(ready.at("protocol_minor") == 1);
  CHECK(ready.at("capabilities") == nlohmann::json{"data-snapshots"});

  const auto data = process.read_json();
  REQUIRE(data.at("type") == "data");
  const auto &snapshot = data.at("value");
  CHECK(snapshot.at("time").get<std::string>().size() == 5);
  CHECK(snapshot.at("time").get<std::string>().at(2) == ':');
  CHECK(snapshot.at("weekdays").size() == 7);
  CHECK(snapshot.at("weeks").size() == 6);
  CHECK(snapshot.at("weeks").front().size() == 7);

  const auto config =
      gisland::load_config(std::filesystem::path{GISLAND_TEST_ASSET_ROOT} / "config.toml");
  REQUIRE(config.has_value());
  const auto &configured_view = config->modules.front().view;
  if (!configured_view) {
    throw std::runtime_error("distributed clock-calendar view is missing");
  }
  const auto &view = *configured_view;
  gisland::ModuleViewState state{view.compact, view.expanded};
  CHECK(state.apply(snapshot).has_value());

  process.send(
      gisland::serialize_core_message(gisland::ShutdownMessage{.reason = "test", .deadline = 1s}));
  CHECK(process.wait_for_exit().success());
}

TEST_CASE("clock-calendar process resolves options and handles calendar actions") {
  ModuleProcess process;
  process.send(gisland::serialize_core_message(
      init({{"timezone", "Etc/GMT-2"}, {"locale", "C"}, {"week_start", "sunday"}})));
  static_cast<void>(process.read_json());
  const auto initial = process.read_json();
  CHECK(initial.at("value").at("weekdays").front() == "Sun");
  const auto initial_month = initial.at("value").at("month_label");

  process.send(gisland::serialize_core_message(
      gisland::ActionMessage{.action_id = "previous-month", .value = std::nullopt}));
  const auto accepted = process.read_json();
  CHECK(accepted.at("type") == "action_result");
  CHECK(accepted.at("accepted") == true);
  const auto changed = process.read_json();
  CHECK(changed.at("type") == "data");
  CHECK(changed.at("value").at("month_label") != initial_month);

  for (const std::string action_id : {"next-month", "today"}) {
    process.send(gisland::serialize_core_message(
        gisland::ActionMessage{.action_id = action_id, .value = std::nullopt}));
    CHECK(process.read_json().at("accepted") == true);
    CHECK(process.read_json().at("type") == "data");
  }

  process.send(gisland::serialize_core_message(
      gisland::ActionMessage{.action_id = "unknown", .value = std::nullopt}));
  const auto rejected = process.read_json();
  CHECK(rejected.at("accepted") == false);
  CHECK_FALSE(process.output_ready(100ms));

  process.send(
      gisland::serialize_core_message(gisland::ShutdownMessage{.reason = "test", .deadline = 1s}));
  CHECK(process.wait_for_exit().success());
}

TEST_CASE("clock-calendar process rejects incompatible initialization") {
  ModuleProcess process;
  auto message = init();
  message.capabilities.clear();
  process.send(gisland::serialize_core_message(message));

  const auto status = process.wait_for_exit();
  CHECK_FALSE(status.success());
}

TEST_CASE("clock-calendar process rejects invalid module options") {
  SECTION("timezone") {
    ModuleProcess process;
    process.send(gisland::serialize_core_message(init({{"timezone", "not/a-zone"}})));
    CHECK_FALSE(process.wait_for_exit().success());
  }

  SECTION("week start") {
    ModuleProcess process;
    process.send(gisland::serialize_core_message(init({{"week_start", "tuesday"}})));
    CHECK_FALSE(process.wait_for_exit().success());
  }
}
