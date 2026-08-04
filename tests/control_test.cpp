#include "gisland/control.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

TEST_CASE("control requests parse into exact typed commands") {
  const std::vector<std::pair<std::string, gisland::ControlCommand>> requests{
      {R"({"version":1,"command":"open"})", gisland::OpenControl{}},
      {R"({"version":1,"command":"close"})", gisland::CloseControl{}},
      {R"({"version":1,"command":"toggle"})", gisland::ToggleControl{}},
      {R"({"version":1,"command":"status"})", gisland::StatusControl{}},
      {R"({"version":1,"command":"modules"})", gisland::ModulesControl{}},
      {R"({"version":1,"command":"module-restart","instance":"clock"})",
       gisland::RestartModuleControl{"clock"}},
      {R"({"version":1,"command":"activate","instance":"clock"})",
       gisland::ActivateControl{"clock", std::nullopt}},
      {R"({"version":1,"command":"activate","instance":"clock","duration_ms":5000})",
       gisland::ActivateControl{"clock", 5s}},
      {R"({"version":1,"command":"dismiss","context_id":"notice"})",
       gisland::DismissControl{"notice"}},
  };

  for (const auto &[line, expected] : requests) {
    CAPTURE(line);
    const auto parsed = gisland::parse_control_request(line);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == expected);
    CHECK(gisland::serialize_control_request(*parsed) == line + "\n");
  }
}

TEST_CASE("control request validation has deterministic precedence") {
  SECTION("syntax precedes shape") {
    const auto parsed = gisland::parse_control_request("{");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == gisland::ControlErrorCode::invalid_request);
  }

  SECTION("duplicate keys precede version") {
    const auto parsed =
        gisland::parse_control_request(R"({"version":2,"command":"open","command":"close"})");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == gisland::ControlErrorCode::invalid_request);
  }

  SECTION("version precedes command") {
    const auto parsed =
        gisland::parse_control_request(R"({"version":2,"command":"missing","extra":true})");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == gisland::ControlErrorCode::unsupported_version);
  }

  SECTION("command precedes exact fields") {
    const auto parsed =
        gisland::parse_control_request(R"({"version":1,"command":"missing","extra":true})");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == gisland::ControlErrorCode::unknown_command);
  }

  SECTION("unknown fields precede semantic values") {
    const auto parsed = gisland::parse_control_request(
        R"({"version":1,"command":"activate","instance":"","duration_ms":0,"extra":true})");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == gisland::ControlErrorCode::invalid_request);
  }
}

TEST_CASE("control requests reject missing types and duration bounds") {
  for (const std::string line : {
           R"([])",
           R"({"version":1})",
           R"({"version":1,"command":"open","extra":true})",
           R"({"version":1,"command":"module-restart","instance":3})",
           R"({"version":1,"command":"activate","instance":"clock","duration_ms":0})",
           R"({"version":1,"command":"activate","instance":"clock","duration_ms":86400001})",
           R"({"version":1,"command":"dismiss","context_id":""})",
       }) {
    CAPTURE(line);
    CHECK_FALSE(gisland::parse_control_request(line).has_value());
  }
}

TEST_CASE("control responses serialize stable success error and status shapes") {
  const gisland::ControlStatus status{
      .mode = gisland::IslandMode::expanded,
      .active_context = gisland::ActiveContextStatus{"clock", "configured", 0},
      .modules = {{"clock", gisland::ControlModuleState::running, true},
                  {"weather", gisland::ControlModuleState::disabled, false}},
      .socket = "/run/user/1000/gisland.sock",
  };
  const auto success =
      nlohmann::json::parse(gisland::serialize_control_response(gisland::ControlResponse{status}));
  CHECK(success.at("version") == 1);
  CHECK(success.at("ok") == true);
  CHECK(success.at("result").at("format_version") == 1);
  CHECK(success.at("result").at("mode") == "expanded");
  CHECK(success.at("result").at("active_context").at("context_id") == "configured");
  CHECK(success.at("result").at("modules").at(1).at("state") == "disabled");

  const auto modules = nlohmann::json::parse(gisland::serialize_control_response(
      gisland::ControlResponse{gisland::ModulesStatus{status.modules}}));
  CHECK(modules.at("result").size() == 1);
  CHECK(modules.at("result").at("modules").size() == 2);

  const auto error =
      nlohmann::json::parse(gisland::serialize_control_response(gisland::ControlResponse{
          gisland::ControlError{gisland::ControlErrorCode::unknown_instance, "unknown module"}}));
  CHECK(error.at("ok") == false);
  CHECK(error.at("error").at("code") == "unknown_instance");
  CHECK(error.at("error").at("message") == "unknown module");
}

TEST_CASE("control responses parse back into typed values and reject malformed envelopes") {
  const gisland::ControlResponse original{gisland::ControlStatus{
      .mode = gisland::IslandMode::compact,
      .active_context = gisland::ActiveContextStatus{"clock", "configured", 0},
      .modules = {{"clock", gisland::ControlModuleState::running, true}},
      .socket = "/run/user/1000/gisland.sock",
  }};
  const auto parsed =
      gisland::parse_control_response(gisland::serialize_control_response(original));
  REQUIRE(parsed.has_value());
  const auto &status = std::get<gisland::ControlStatus>(parsed->value());
  CHECK(status.mode == gisland::IslandMode::compact);
  CHECK(status.modules == std::vector<gisland::ModuleControlStatus>{
                              {"clock", gisland::ControlModuleState::running, true}});

  for (const std::string &record : {
           std::string{"{}"},
           std::string{R"({"version":2,"ok":true,"result":{}})"},
           std::string{R"({"version":1,"ok":true,"result":{},"extra":1})"},
           std::string{R"({"version":1,"ok":false,"error":{"code":"invented","message":"x"}})"},
       }) {
    CAPTURE(record);
    CHECK_FALSE(gisland::parse_control_response(record).has_value());
  }
}

TEST_CASE("gislandctl grammar parses commands and bounded durations") {
  const auto open = gisland::parse_control_arguments({"open"});
  REQUIRE(open.has_value());
  CHECK(std::holds_alternative<gisland::OpenControl>(open->command));
  CHECK_FALSE(open->json_output);

  const auto status = gisland::parse_control_arguments({"status", "--json"});
  REQUIRE(status.has_value());
  CHECK(std::holds_alternative<gisland::StatusControl>(status->command));
  CHECK(status->json_output);

  const auto restart = gisland::parse_control_arguments({"module", "restart", "clock"});
  REQUIRE(restart.has_value());
  CHECK(std::get<gisland::RestartModuleControl>(restart->command).instance_id == "clock");

  for (const auto &[value, duration] :
       std::vector<std::pair<std::string, std::chrono::milliseconds>>{
           {"1ms", 1ms}, {"5s", 5s}, {"2m", 2min}, {"24h", 24h}}) {
    const auto parsed =
        gisland::parse_control_arguments({"activate", "clock", "--duration", value});
    REQUIRE(parsed.has_value());
    CHECK(std::get<gisland::ActivateControl>(parsed->command).duration == duration);
  }

  for (const std::vector<std::string> &arguments : {
           std::vector<std::string>{},
           {"reload"},
           {"open", "--json"},
           {"module", "restart"},
           {"activate", "clock", "--duration", "0s"},
           {"activate", "clock", "--duration", "1.5s"},
           {"activate", "clock", "--duration", "25h"},
           {"dismiss", ""},
       }) {
    CAPTURE(arguments);
    CHECK_FALSE(gisland::parse_control_arguments(arguments).has_value());
  }
}
