#include "gisland/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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

TEST_CASE("a publish line decodes context-owned RGBA8 image resources") {
  constexpr auto line = R"({
    "type": "publish",
    "context_id": "notification",
    "priority": 20,
    "resources": [
      {"id":"app-icon","format":"rgba8","width":1,"height":1,"data":"/wAA/w=="}
    ],
    "compact": {
      "type":"image",
      "resource_id":"app-icon",
      "role":"notification-icon",
      "accessible_label":"Firefox"
    }
  })";

  const auto result = gisland::parse_module_message(line);

  REQUIRE(result.has_value());
  const auto *publish = std::get_if<gisland::PublishMessage>(&*result);
  REQUIRE(publish != nullptr);
  REQUIRE(publish->resources.size() == 1);
  const auto &resource = publish->resources.front();
  CHECK(resource.id == "app-icon");
  CHECK(resource.format == gisland::ImageFormat::rgba8);
  CHECK(resource.width == 1);
  CHECK(resource.height == 1);
  REQUIRE(resource.pixels != nullptr);
  CHECK(*resource.pixels == std::vector<std::uint8_t>{255, 0, 0, 255});
  const auto *image = std::get_if<gisland::Image>(&publish->compact.value);
  REQUIRE(image != nullptr);
  CHECK(image->resource_id == "app-icon");
  CHECK(image->role == "notification-icon");
}

TEST_CASE("a publish line parses structured rich content and action regions") {
  constexpr auto line = R"({
    "type":"publish",
    "context_id":"notification",
    "priority":20,
    "resources":[{"id":"preview","format":"rgba8","width":1,"height":1,"data":"/wAA/w=="}],
    "compact":{"type":"text","value":"Download complete","role":"compact-primary"},
    "expanded":{
      "type":"action_region",
      "action_id":"default",
      "accessible_label":"Open notification",
      "content":{
        "type":"rich_text",
        "role":"notification-body",
        "content":[
          {"type":"text","value":"The file ","emphasis":[]},
          {"type":"text","value":"archive.tar.gz","emphasis":["bold"]},
          {"type":"link","value":"Open folder","action_id":"link-0","accessible_label":"Open folder"},
          {"type":"inline_image","resource_id":"preview","role":"notification-inline-image","accessible_label":"Preview"}
        ]
      }
    }
  })";

  const auto result = gisland::parse_module_message(line);

  REQUIRE(result.has_value());
  const auto *publish = std::get_if<gisland::PublishMessage>(&*result);
  REQUIRE(publish != nullptr);
  REQUIRE(publish->expanded.has_value());
  const auto *region = std::get_if<gisland::ActionRegion>(&publish->expanded->value);
  REQUIRE(region != nullptr);
  CHECK(region->action_id == "default");
  const auto *rich = std::get_if<gisland::RichText>(&region->content->value);
  REQUIRE(rich != nullptr);
  REQUIRE(rich->content.size() == 4);
  CHECK(std::get<gisland::RichTextSpan>(rich->content[1]).emphasis ==
        std::vector<gisland::TextEmphasis>{gisland::TextEmphasis::bold});
  CHECK(std::get<gisland::RichLinkSpan>(rich->content[2]).action_id == "link-0");
  CHECK(std::get<gisland::RichInlineImage>(rich->content[3]).resource_id == "preview");
}

TEST_CASE("image resources reject malformed payloads and references") {
  const auto check = [](std::string_view resources, std::string_view resource_id,
                        std::string_view expected_path) {
    const auto line =
        std::string{R"({"type":"publish","context_id":"x","priority":0,"resources":)"} +
        std::string{resources} + R"(,"compact":{"type":"image","resource_id":")" +
        std::string{resource_id} + R"(","role":"notification-icon","accessible_label":"Icon"}})";
    const auto result = gisland::parse_module_message(line);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == expected_path);
  };

  SECTION("strict base64") {
    check(R"([{"id":"icon","format":"rgba8","width":1,"height":1,"data":"@@=="}])", "icon",
          "/resources/0/data");
  }
  SECTION("positive dimensions") {
    check(R"([{"id":"icon","format":"rgba8","width":0,"height":1,"data":""}])", "icon",
          "/resources/0/width");
  }
  SECTION("exact byte count") {
    check(R"([{"id":"icon","format":"rgba8","width":2,"height":1,"data":"/wAA/w=="}])", "icon",
          "/resources/0/data");
  }
  SECTION("unique identifiers") {
    check(
        R"([{"id":"icon","format":"rgba8","width":1,"height":1,"data":"/wAA/w=="},{"id":"icon","format":"rgba8","width":1,"height":1,"data":"/wAA/w=="}])",
        "icon", "/resources/1/id");
  }
  SECTION("local references") {
    check(R"([{"id":"other","format":"rgba8","width":1,"height":1,"data":"/wAA/w=="}])", "icon",
          "/compact/resource_id");
  }
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
  CHECK(data->value ==
        nlohmann::json{{"temperature", 21.5},
                       {"forecast", {{{"day", "Tuesday"}, {"hours", {14, 15, 16}}}}}});
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

  SECTION("oversized scene semantic identifier") {
    const std::string oversized_identifier(129, 'x');
    const auto line =
        std::string{
            R"({"type":"publish","context_id":"x","priority":0,"compact":{"type":"icon","name":")"} +
        oversized_identifier + R"(","accessible_label":"Clock"}})";
    const auto result = gisland::parse_module_message(line);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "/compact/name");
    CHECK(result.error().message == "scene identifier exceeds maximum byte count");
  }

  SECTION("oversized scene label") {
    const std::string oversized_label(4097, 'x');
    const auto line =
        std::string{
            R"({"type":"publish","context_id":"x","priority":0,"compact":{"type":"progress","value":0.5,"label":")"} +
        oversized_label + R"(","state":"accent"}})";
    const auto result = gisland::parse_module_message(line);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "/compact/label");
    CHECK(result.error().message == "text exceeds maximum byte count");
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
