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
      {R"({"version":1,"command":"reload"})", gisland::ReloadControl{}},
      {R"({"version":1,"command":"module-restart","instance":"clock"})",
       gisland::RestartModuleControl{"clock"}},
      {R"({"version":1,"command":"activate","instance":"clock"})",
       gisland::ActivateControl{"clock", std::nullopt}},
      {R"({"version":1,"command":"activate","instance":"clock","duration_ms":5000})",
       gisland::ActivateControl{"clock", 5s}},
      {R"({"version":1,"command":"activate-open","instance":"notifications"})",
       gisland::ActivateOpenControl{"notifications"}},
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
      .compact = gisland::ActiveContextStatus{"clock", "configured", 0},
      .expanded = gisland::ActiveContextStatus{"calendar", "configured", 0},
      .modules = {{"clock", gisland::ControlModuleState::running, true},
                  {"weather", gisland::ControlModuleState::disabled, false}},
      .socket = "/run/user/1000/gisland.sock",
  };
  const auto success =
      nlohmann::json::parse(gisland::serialize_control_response(gisland::ControlResponse{status}));
  CHECK(success.at("version") == 1);
  CHECK(success.at("ok") == true);
  CHECK(success.at("result").at("format_version") == 2);
  CHECK(success.at("result").at("mode") == "expanded");
  CHECK(success.at("result").at("compact").at("instance_id") == "clock");
  CHECK(success.at("result").at("expanded").at("instance_id") == "calendar");
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

  auto compact_only = status;
  compact_only.expanded.reset();
  const auto compact_only_json = nlohmann::json::parse(
      gisland::serialize_control_response(gisland::ControlResponse{compact_only}));
  CHECK(compact_only_json.at("result").at("expanded").is_null());
}

TEST_CASE("control responses parse back into typed values and reject malformed envelopes") {
  const gisland::ControlResponse original{gisland::ControlStatus{
      .mode = gisland::IslandMode::compact,
      .compact = gisland::ActiveContextStatus{"clock", "configured", 0},
      .expanded = gisland::ActiveContextStatus{"clock", "configured", 0},
      .modules = {{"clock", gisland::ControlModuleState::running, true},
                  {"weather", gisland::ControlModuleState::disabled, false}},
      .socket = "/run/user/1000/gisland.sock",
  }};
  const auto parsed =
      gisland::parse_control_response(gisland::serialize_control_response(original));
  REQUIRE(parsed.has_value());
  const auto &status = std::get<gisland::ControlStatus>(parsed->value());
  CHECK(status.mode == gisland::IslandMode::compact);
  REQUIRE(status.compact.has_value());
  REQUIRE(status.expanded.has_value());
  CHECK(status.compact->instance_id == "clock");
  CHECK(status.modules == std::vector<gisland::ModuleControlStatus>{
                              {"clock", gisland::ControlModuleState::running, true},
                              {"weather", gisland::ControlModuleState::disabled, false}});

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

TEST_CASE("control response parser accepts legacy status format one") {
  const auto parsed = gisland::parse_control_response(
      R"({"version":1,"ok":true,"result":{"format_version":1,"mode":"compact","active_context":{"instance_id":"clock","context_id":"configured","priority":0},"modules":[],"socket":"/tmp/gisland.sock"}})");
  REQUIRE(parsed.has_value());
  const auto &status = std::get<gisland::ControlStatus>(parsed->value());
  REQUIRE(status.compact.has_value());
  CHECK(status.compact->instance_id == "clock");
  CHECK_FALSE(status.expanded.has_value());
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

  const auto reload = gisland::parse_control_arguments({"reload"});
  REQUIRE(reload.has_value());
  CHECK(std::holds_alternative<gisland::ReloadControl>(reload->command));

  const auto activate_open = gisland::parse_control_arguments({"activate-open", "notifications"});
  REQUIRE(activate_open.has_value());
  CHECK(std::get<gisland::ActivateOpenControl>(activate_open->command).instance_id ==
        "notifications");

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
           {"reload", "extra"},
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

TEST_CASE("gislandctl action grammar preserves optional typed JSON values") {
  const auto valueless = gisland::parse_control_arguments({"action", "audio", "volume-up"});
  REQUIRE(valueless.has_value());
  const auto &plain = std::get<gisland::ActionControl>(valueless->command);
  CHECK(plain.instance_id == "audio");
  CHECK(plain.action_id == "volume-up");
  CHECK_FALSE(plain.value.has_value());

  for (const auto &value :
       {"null", "true", "42", "2.5", R"("quiet")", "[1,false]", R"({"level":3})"}) {
    CAPTURE(value);
    const auto parsed =
        gisland::parse_control_arguments({"action", "audio", "set", "--value", value});
    REQUIRE(parsed.has_value());
    const auto &action = std::get<gisland::ActionControl>(parsed->command);
    REQUIRE(action.value.has_value());
    CHECK(*action.value == nlohmann::json::parse(value));
  }
}

TEST_CASE("action grammar rejects empty identifiers malformed JSON and invalid option shape") {
  const std::string oversized(64 * 1024, 'x');
  for (const std::vector<std::string> &arguments : {
           std::vector<std::string>{"action", "", "set"},
           {"action", "audio", ""},
           {"action", "audio"},
           {"action", "audio", "set", "--value"},
           {"action", "audio", "set", "--value", "{"},
           {"action", "audio", "set", "--value", R"({"x":1,"x":2})"},
           {"action", "audio", "set", "--value", "1", "--value", "2"},
           {"action", "audio", "set", "--other", "1"},
           {"action", "audio", "set", "--value", oversized},
       }) {
    CAPTURE(arguments);
    CHECK_FALSE(gisland::parse_control_arguments(arguments).has_value());
  }
}

TEST_CASE("action control requests round trip optional values including null") {
  for (const gisland::ActionControl &action : {
           gisland::ActionControl{"audio", "toggle-mute", std::nullopt},
           gisland::ActionControl{"audio", "set", nlohmann::json(nullptr)},
           gisland::ActionControl{"audio", "set", nlohmann::json{{"level", 3}}},
       }) {
    const gisland::ControlCommand command{action};
    const auto serialized = gisland::serialize_control_request(command);
    const auto parsed = gisland::parse_control_request(serialized);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == command);
  }
}
