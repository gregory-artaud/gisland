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

  REQUIRE(publish->compact.has_value());
  const auto *compact = std::get_if<gisland::Text>(&publish->compact->value);
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

TEST_CASE("protocol 1.4 parses independent views and presentation intent") {
  constexpr auto line = R"({
    "type":"publish","context_id":"alert","priority":10,
    "views":{"expanded":{"type":"text","value":"Alert","role":"body"}},
    "presentation":{"reveal":"expanded","duration_ms":1000}
  })";

  const auto result = gisland::parse_module_message(line);

  REQUIRE(result.has_value());
  const auto *publish = std::get_if<gisland::PublishMessage>(&*result);
  REQUIRE(publish != nullptr);
  CHECK_FALSE(publish->compact.has_value());
  REQUIRE(publish->expanded.has_value());
  REQUIRE(publish->presentation.has_value());
  CHECK(publish->presentation->duration == std::chrono::milliseconds{1000});
}

TEST_CASE("protocol 1.7 parses compact HUD style icon role and progress source") {
  constexpr auto line = R"({
    "type":"publish","context_id":"audio-volume","priority":80,"expires_in_ms":1500,
    "views":{"compact":{"type":"row","children":[
      {"type":"icon","name":"volume-low","accessible_label":"Volume","role":"hud-icon"},
      {"type":"progress","value":0.5,"transition_from":0.4,"state":"accent"}
    ]}},
    "presentation":{"compact_style":"hud-meter"}
  })";

  const auto result = gisland::parse_module_message(line);

  REQUIRE(result.has_value());
  const auto *publish = std::get_if<gisland::PublishMessage>(&*result);
  REQUIRE(publish != nullptr);
  REQUIRE(publish->presentation.has_value());
  CHECK(publish->presentation->compact_style == "hud-meter");
  CHECK_FALSE(publish->presentation->reveal.has_value());
  REQUIRE(publish->compact.has_value());
  const auto &row = std::get<gisland::Row>(publish->compact->value);
  CHECK(std::get<gisland::Icon>(row.children[0]->value).role == "hud-icon");
  CHECK(std::get<gisland::Progress>(row.children[1]->value).transition_from == 0.4);
}

TEST_CASE("protocol 1.7 rejects invalid progress transition source") {
  const auto result = gisland::parse_module_message(
      R"({"type":"publish","context_id":"audio","priority":80,"views":{"compact":{"type":"progress","value":0.5,"transition_from":1.1}}})");

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().path == "/views/compact/transition_from");
}

TEST_CASE("protocol 1.4 requires at least one independent view") {
  const auto result = gisland::parse_module_message(
      R"({"type":"publish","context_id":"empty","priority":0,"views":{}})");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().path == "/views");
}

TEST_CASE("protocol 1.4 validates presentation and independent view shape strictly") {
  for (
      const auto &[line, path] : std::vector<std::pair<std::string, std::string>>{
          {R"({"type":"publish","context_id":"x","priority":0,"views":{"other":{}}})",
           "/views/other"},
          {R"({"type":"publish","context_id":"x","priority":0,"views":{"compact":{"type":"text","value":"x","role":"body"}},"presentation":{"reveal":"expanded"}})",
           "/presentation"},
          {R"({"type":"publish","context_id":"x","priority":0,"views":{"expanded":{"type":"text","value":"x","role":"body"}},"presentation":{"reveal":"expanded","duration_ms":0}})",
           "/presentation/duration_ms"},
          {R"({"type":"publish","context_id":"x","priority":0,"views":{"expanded":{"type":"text","value":"x","role":"body"}},"presentation":{"reveal":"compact"}})",
           "/presentation/reveal"},
      }) {
    CAPTURE(line);
    const auto result = gisland::parse_module_message(line);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == path);
  }
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
        {"type": "progress", "value": 0.75, "label": "75%", "state": "success", "shape": "ring"},
        {"type": "indicator", "state": "success", "accessible_label": "Available"},
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
  REQUIRE(publish->compact.has_value());
  const auto *row = std::get_if<gisland::Row>(&publish->compact->value);
  REQUIRE(row != nullptr);
  REQUIRE(row->children.size() == 5);
  CHECK(std::holds_alternative<gisland::Icon>(row->children[0]->value));
  CHECK(std::holds_alternative<gisland::Spacer>(row->children[1]->value));
  const auto *progress = std::get_if<gisland::Progress>(&row->children[2]->value);
  REQUIRE(progress != nullptr);
  CHECK(progress->shape == gisland::ProgressShape::ring);
  const auto *indicator = std::get_if<gisland::Indicator>(&row->children[3]->value);
  REQUIRE(indicator != nullptr);
  CHECK(indicator->state == "success");
  CHECK(indicator->accessible_label == "Available");
  CHECK(std::holds_alternative<gisland::Button>(row->children[4]->value));
}

TEST_CASE("indicator protocol fields are required") {
  for (const auto &[node, path] : std::vector<std::pair<std::string, std::string>>{
           {R"({"type":"indicator","accessible_label":"Available"})", "/compact/state"},
           {R"({"type":"indicator","state":"success"})", "/compact/accessible_label"},
       }) {
    const auto result = gisland::parse_module_message(
        "{\"type\":\"publish\",\"context_id\":\"x\",\"priority\":0,\"compact\":" + node + "}");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == path);
  }
}

TEST_CASE("protocol 1.9 parses combinable semantic indicator effects") {
  const auto result = gisland::parse_module_message(
      R"({"type":"publish","context_id":"job","priority":20,"compact":{"type":"indicator","state":"success","accessible_label":"Running","effects":["shadow","glow","breathe"]}})");

  REQUIRE(result.has_value());
  const auto *publish = std::get_if<gisland::PublishMessage>(&*result);
  REQUIRE(publish != nullptr);
  REQUIRE(publish->compact.has_value());
  const auto &indicator = std::get<gisland::Indicator>(publish->compact->value);
  CHECK(indicator.effects == std::vector<gisland::IndicatorEffect>{
                                 gisland::IndicatorEffect::shadow, gisland::IndicatorEffect::glow,
                                 gisland::IndicatorEffect::breathe});
}

TEST_CASE("indicator effects reject malformed unknown and duplicate requests") {
  for (const auto &[effects, path] : std::vector<std::pair<std::string, std::string>>{
           {R"("glow")", "/compact/effects"},
           {R"(["sparkle"])", "/compact/effects/0"},
           {R"(["glow","glow"])", "/compact/effects/1"},
           {R"(["glow",1])", "/compact/effects/1"},
       }) {
    const auto result = gisland::parse_module_message(
        "{\"type\":\"publish\",\"context_id\":\"x\",\"priority\":0,\"compact\":{"
        "\"type\":\"indicator\",\"state\":\"success\",\"accessible_label\":\"Available\","
        "\"effects\":" +
        effects + "}}");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == path);
  }
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
  REQUIRE(publish->compact.has_value());
  const auto *image = std::get_if<gisland::Image>(&publish->compact->value);
  REQUIRE(image != nullptr);
  CHECK(image->resource_id == "app-icon");
  CHECK(image->role == "notification-icon");
}

TEST_CASE("core parser accepts image base64 larger than generic scene text") {
  const std::string encoded(8192, 'A');
  const std::string line =
      R"({"type":"publish","context_id":"x","priority":0,"resources":[{"id":"image","format":"rgba8","width":64,"height":24,"data":")" +
      encoded +
      R"("}],"compact":{"type":"image","resource_id":"image","role":"image","accessible_label":"Image"}})";

  const auto result = gisland::parse_module_message(line);

  REQUIRE(result.has_value());
  const auto *publish = std::get_if<gisland::PublishMessage>(&*result);
  REQUIRE(publish != nullptr);
  REQUIRE(publish->resources.size() == 1);
  CHECK(publish->resources[0].pixels->size() == 64U * 24U * 4U);
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
  CHECK_FALSE(typed_action_result->invocation_id.has_value());

  const auto correlated_action_result = gisland::parse_module_message(
      R"({"type":"action_result","action_id":"audio.volume-up","invocation_id":"18446744073709551615","accepted":true})");
  REQUIRE(correlated_action_result.has_value());
  const auto *typed_correlated =
      std::get_if<gisland::ActionResultMessage>(&*correlated_action_result);
  REQUIRE(typed_correlated != nullptr);
  CHECK(typed_correlated->invocation_id == 18446744073709551615ULL);

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

  SECTION("invalid action result invocation identifier") {
    for (const std::string_view invocation_id : {"", "-1", "12x", "18446744073709551616"}) {
      const auto result = gisland::parse_module_message(
          std::string{
              R"({"type":"action_result","action_id":"audio.volume-up","accepted":true,"invocation_id":")"} +
          std::string{invocation_id} + R"("})");
      REQUIRE_FALSE(result.has_value());
      CHECK(result.error().path == "/invocation_id");
    }
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
