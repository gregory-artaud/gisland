#include "gisland/control_client.hpp"
#include "gisland/ipc_server.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <ranges>
#include <string>
#include <thread>
#include <unistd.h>
#include <variant>

using namespace std::chrono_literals;

namespace {

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    std::array<char, 64> pattern{};
    const std::string source = "/tmp/gisland-client-test-XXXXXX";
    std::ranges::copy(source, pattern.begin());
    path_ = ::mkdtemp(pattern.data());
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
  [[nodiscard]] const std::string &path() const { return path_; }

private:
  std::string path_;
};

} // namespace

TEST_CASE("control client exchanges one typed command with the Unix server") {
  TemporaryDirectory directory;
  auto server = gisland::IpcServer::create(directory.path());
  REQUIRE(server.has_value());
  std::atomic_bool stop{false};
  std::jthread server_thread{[&] {
    while (!stop.load()) {
      server->advance(std::chrono::steady_clock::now(), [](const gisland::ControlCommand &command) {
        CHECK(std::holds_alternative<gisland::OpenControl>(command));
        return gisland::ControlResponse{};
      });
      std::this_thread::yield();
    }
  }};

  const auto response =
      gisland::send_control_command(server->socket_path(), gisland::OpenControl{});
  stop = true;
  REQUIRE(response.has_value());
  CHECK(std::holds_alternative<gisland::EmptyControlResult>(response->value()));
  CHECK(gisland::format_control_output(*response, false) == "ok\n");
}

TEST_CASE("control client reports unavailable and malformed peers") {
  TemporaryDirectory directory;
  const auto missing =
      gisland::send_control_command(directory.path() + "/missing.sock", gisland::StatusControl{});
  REQUIRE_FALSE(missing.has_value());
  CHECK_FALSE(missing.error().message.empty());
}

TEST_CASE("control client formats stable JSON status results") {
  const gisland::ControlResponse response{gisland::ControlStatus{
      .mode = gisland::IslandMode::expanded,
      .compact = gisland::ActiveContextStatus{"clock", "configured", 0},
      .expanded = gisland::ActiveContextStatus{"calendar", "configured", 0},
      .modules = {{"clock", gisland::ControlModuleState::running, true}},
      .socket = "/run/user/1000/gisland.sock",
  }};
  const auto output = nlohmann::json::parse(gisland::format_control_output(response, true));
  CHECK(output.at("format_version") == 2);
  CHECK(output.at("mode") == "expanded");
  CHECK(output.at("modules").at(0).at("id") == "clock");
}

TEST_CASE("control client gives only action responses an extended read deadline") {
  CHECK(gisland::control_read_timeout(gisland::OpenControl{}) == 2s);
  CHECK(gisland::control_read_timeout(gisland::ActionControl{"audio", "volume-up", std::nullopt}) ==
        3s);
}
