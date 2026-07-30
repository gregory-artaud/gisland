#include "gisland/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <variant>

TEST_CASE("a publish line is parsed into typed scenes") {
  constexpr auto line = R"({
    "type": "publish",
    "context_id": "clock",
    "priority": 7,
    "expires_in_ms": 1500,
    "compact": {
      "type": "text",
      "value": "14:32",
      "role": "body",
      "truncation": "end"
    },
    "expanded": {
      "type": "column",
      "alignment": "center",
      "gap": "normal",
      "children": [
        {"type": "text", "value": "Tuesday", "role": "title"}
      ]
    }
  })";

  const auto result = gisland::parse_module_message(line);

  REQUIRE(result.has_value());
  const auto *publish = std::get_if<gisland::PublishMessage>(&*result);
  REQUIRE(publish != nullptr);
  CHECK(publish->context_id == "clock");
  CHECK(publish->priority == 7);
  REQUIRE(publish->expires_in.has_value());
  CHECK(publish->expires_in.value_or(std::chrono::milliseconds{-1}) ==
        std::chrono::milliseconds{1500});

  const auto *compact = std::get_if<gisland::Text>(&publish->compact.value);
  REQUIRE(compact != nullptr);
  CHECK(compact->value == "14:32");
  CHECK(compact->role == "body");
  CHECK(compact->truncation == "end");

  REQUIRE(publish->expanded.has_value());
  const auto expanded_scene =
      publish->expanded.value_or(gisland::SceneNode{gisland::Text{"missing", "body"}});
  const auto *expanded = std::get_if<gisland::Column>(&expanded_scene.value);
  REQUIRE(expanded != nullptr);
  REQUIRE(expanded->children.size() == 1);
  CHECK(std::holds_alternative<gisland::Text>(expanded->children.front()->value));
}

TEST_CASE("all v1 scene discriminators parse at the protocol boundary") {
  constexpr auto line = R"({
    "type": "publish",
    "context_id": "complete-scene",
    "priority": 0,
    "compact": {
      "type": "row",
      "children": [
        {"type": "icon", "name": "clock", "accessible_label": "Clock"},
        {"type": "spacer", "flexible": false, "size_token": "small"},
        {"type": "progress", "value": 0.75, "label": "75%", "state": "success"},
        {
          "type": "button",
          "content": {"type": "text", "value": "Open", "role": "label"},
          "action_id": "open",
          "enabled": true,
          "accessible_label": "Open details"
        }
      ]
    }
  })";

  const auto result = gisland::parse_module_message(line);

  REQUIRE(result.has_value());
  const auto *publish = std::get_if<gisland::PublishMessage>(&*result);
  REQUIRE(publish != nullptr);
  const auto *row = std::get_if<gisland::Row>(&publish->compact.value);
  REQUIRE(row != nullptr);
  REQUIRE(row->children.size() == 4);
  CHECK(std::holds_alternative<gisland::Icon>(row->children[0]->value));
  CHECK(std::holds_alternative<gisland::Spacer>(row->children[1]->value));
  CHECK(std::holds_alternative<gisland::Progress>(row->children[2]->value));
  CHECK(std::holds_alternative<gisland::Button>(row->children[3]->value));
}

TEST_CASE("ready dismiss action-result and log lines are parsed into typed messages") {
  const auto ready = gisland::parse_module_message(
      R"({"type":"ready","protocol_major":1,"protocol_minor":2,"capabilities":["actions","visibility"]})");
  REQUIRE(ready.has_value());
  const auto *ready_message = std::get_if<gisland::ReadyMessage>(&*ready);
  REQUIRE(ready_message != nullptr);
  CHECK(ready_message->protocol_major == 1);
  CHECK(ready_message->protocol_minor == 2);
  CHECK((ready_message->capabilities == std::vector<std::string>{"actions", "visibility"}));

  const auto dismiss = gisland::parse_module_message(R"({"type":"dismiss","context_id":"clock"})");
  REQUIRE(dismiss.has_value());
  const auto *dismiss_message = std::get_if<gisland::DismissMessage>(&*dismiss);
  REQUIRE(dismiss_message != nullptr);
  CHECK(dismiss_message->context_id == "clock");

  const auto action_result = gisland::parse_module_message(
      R"({"type":"action_result","action_id":"calendar.today","accepted":false,"message":"not available"})");
  REQUIRE(action_result.has_value());
  const auto *typed_action_result = std::get_if<gisland::ActionResultMessage>(&*action_result);
  REQUIRE(typed_action_result != nullptr);
  CHECK(typed_action_result->action_id == "calendar.today");
  CHECK_FALSE(typed_action_result->accepted);
  CHECK(typed_action_result->message == "not available");

  const auto log =
      gisland::parse_module_message(R"({"type":"log","level":"warning","message":"late update"})");
  REQUIRE(log.has_value());
  const auto *typed_log = std::get_if<gisland::LogMessage>(&*log);
  REQUIRE(typed_log != nullptr);
  CHECK(typed_log->level == gisland::LogLevel::warning);
  CHECK(typed_log->message == "late update");
}

TEST_CASE("a data line preserves its complete object value") {
  const auto result = gisland::parse_module_message(
      R"({"type":"data","value":{"temperature":21.5,"forecast":[{"day":"Tuesday","hours":[14,15,16]}]}})");

  REQUIRE(result.has_value());
  const auto *data = std::get_if<gisland::DataMessage>(&*result);
  REQUIRE(data != nullptr);
  CHECK(data->value == nlohmann::json{{"temperature", 21.5},
                                      {"forecast",
                                       {{{"day", "Tuesday"}, {"hours", {14, 15, 16}}}}}});
}

TEST_CASE("a data line requires an object value") {
  SECTION("missing value") {
    const auto result = gisland::parse_module_message(R"({"type":"data"})");

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "/value");
  }

  SECTION("non-object value") {
    const auto result = gisland::parse_module_message(R"({"type":"data","value":[1,2,3]})");

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "/value");
  }
}

TEST_CASE("protocol errors identify the failing JSON path") {
  SECTION("missing required field") {
    const auto result = gisland::parse_module_message(
        R"({"type":"publish","priority":0,"compact":{"type":"text","value":"x","role":"body"}})");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "/context_id");
  }

  SECTION("wrong field type") {
    const auto result = gisland::parse_module_message(
        R"({"type":"publish","context_id":"x","priority":"high","compact":{"type":"text","value":"x","role":"body"}})");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "/priority");
  }

  SECTION("unknown primitive") {
    const auto result = gisland::parse_module_message(
        R"({"type":"publish","context_id":"x","priority":0,"compact":{"type":"canvas"}})");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "/compact/type");
  }

  SECTION("invalid scene") {
    const std::string oversized_text(4097, 'x');
    const auto line =
        std::string{
            R"({"type":"publish","context_id":"x","priority":0,"compact":{"type":"text","value":")"} +
        oversized_text + R"(","role":"body"}})";
    const auto result = gisland::parse_module_message(line);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "/compact/value");
  }

  SECTION("negative expiration") {
    const auto result = gisland::parse_module_message(
        R"({"type":"publish","context_id":"x","priority":0,"expires_in_ms":-1,"compact":{"type":"text","value":"x","role":"body"}})");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "/expires_in_ms");
  }

  SECTION("unknown message") {
    const auto result = gisland::parse_module_message(R"({"type":"surprise"})");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "/type");
  }

  SECTION("duplicate ready capability") {
    const auto result = gisland::parse_module_message(
        R"({"type":"ready","protocol_major":1,"protocol_minor":0,"capabilities":["actions","actions"]})");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "/capabilities/1");
  }

  SECTION("invalid action result") {
    const auto result =
        gisland::parse_module_message(R"({"type":"action_result","action_id":"","accepted":true})");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "/action_id");
  }

  SECTION("unknown log level") {
    const auto result =
        gisland::parse_module_message(R"({"type":"log","level":"verbose","message":"hello"})");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "/level");
  }

  SECTION("malformed JSON") {
    const auto result = gisland::parse_module_message("{");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path.empty());
  }
}
