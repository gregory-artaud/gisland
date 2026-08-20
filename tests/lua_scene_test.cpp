#include "gisland/lua_host.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;
using Records = std::vector<Json>;

class LuaScript {
public:
  explicit LuaScript(std::string_view body) {
    static std::atomic<unsigned> sequence{};
    path_ = std::filesystem::temp_directory_path() /
            ("gisland-lua-scene-" + std::to_string(::getpid()) + "-" + std::to_string(sequence++) +
             ".lua");
    std::ofstream output{path_};
    REQUIRE(output.good());
    output << body;
    REQUIRE(output.good());
  }

  ~LuaScript() { std::filesystem::remove(path_); }

  LuaScript(const LuaScript &) = delete;
  LuaScript &operator=(const LuaScript &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

[[nodiscard]] Json init_record(Json capabilities = Json::array(), int maximum_minor = 8) {
  return {
      {"type", "init"},
      {"protocol",
       {{"minimum", {{"major", 1}, {"minor", 0}}},
        {"maximum", {{"major", 1}, {"minor", maximum_minor}}}}},
      {"instance_id", "scene-test"},
      {"capabilities", std::move(capabilities)},
      {"configuration", Json::object()},
      {"locale", "en_US.UTF-8"},
      {"timezone", "UTC"},
  };
}

[[nodiscard]] gisland::LuaHost::Emit collect_into(Records &records) {
  return [&records](Json record) -> std::expected<void, gisland::LuaHostError> {
    records.push_back(std::move(record));
    return {};
  };
}

[[nodiscard]] std::pair<std::expected<gisland::LuaHostState, gisland::LuaHostError>, Records>
run_script(std::string_view body, Json capabilities = Json::array(), int maximum_minor = 8) {
  LuaScript script{body};
  auto host = gisland::LuaHost::load(script.path().string());
  REQUIRE(host.has_value());
  Records records;
  auto result = (*host)->handle(init_record(std::move(capabilities), maximum_minor),
                                collect_into(records), {});
  return {std::move(result), std::move(records)};
}

[[nodiscard]] std::string publishing(std::string_view expression) {
  return "return gisland.module { init = function() gisland.publish { context_id = 'ctx', "
         "priority = 7, views = { compact = " +
         std::string{expression} + " } } end }";
}

[[nodiscard]] Json emitted_record(std::string_view expression) {
  auto [result, records] = run_script(publishing(expression));
  REQUIRE(result.has_value());
  REQUIRE(records.size() == 2);
  return records[1];
}

} // namespace

TEST_CASE("lua scene API emits exact publish dismiss and log records", "[lua_scene]") {
  const auto [result, records] = run_script(R"lua(
return gisland.module {
  init = function()
    gisland.publish {
      context_id = "audio-volume",
      priority = 80,
      expires_in_ms = 1500,
      views = {
        compact = gisland.ui.text { value = "50%", role = "compact-primary" },
        expanded = gisland.ui.icon {
          name = "volume-high", role = "hud-volume-icon", accessible_label = "Volume"
        }
      },
      resources = {
        { id = "pixel", format = "rgba8", width = 1, height = 1, data = "/wAA/w==" }
      },
      presentation = { reveal = "expanded", duration_ms = 2500, compact_style = "hud-meter" }
    }
    gisland.dismiss("old-context")
    gisland.log("warning", "volume changed")
  end
}
)lua");

  REQUIRE(result.has_value());
  REQUIRE(records.size() == 4);
  CHECK(records[1] ==
        Json{
            {"type", "publish"},
            {"context_id", "audio-volume"},
            {"priority", 80},
            {"expires_in_ms", 1500},
            {"views",
             {{"compact", {{"type", "text"}, {"value", "50%"}, {"role", "compact-primary"}}},
              {"expanded",
               {{"type", "icon"},
                {"name", "volume-high"},
                {"role", "hud-volume-icon"},
                {"accessible_label", "Volume"}}}}},
            {"resources",
             {{{"id", "pixel"},
               {"format", "rgba8"},
               {"width", 1},
               {"height", 1},
               {"data", "/wAA/w=="}}}},
            {"presentation",
             {{"reveal", "expanded"}, {"duration_ms", 2500}, {"compact_style", "hud-meter"}}},
        });
  CHECK(records[2] == Json{{"type", "dismiss"}, {"context_id", "old-context"}});
  CHECK(records[3] == Json{{"type", "log"}, {"level", "warning"}, {"message", "volume changed"}});
}

TEST_CASE("lua publish conversion retains the default 256-item budget", "[lua_scene]") {
  const auto [result, records] = run_script(R"lua(
return gisland.module {
  init = function()
    local context = { context_id = "large", priority = 0 }
    for index = 1, 255 do context["extra" .. index] = index end
    gisland.publish(context)
  end,
}
)lua");

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == gisland::LuaHostErrorCode::callback_error);
  CHECK(result.error().message.find("256") != std::string::npos);
  CHECK(records.empty());
}

TEST_CASE("lua scene constructors emit every protocol primitive exactly", "[lua_scene]") {
  struct Case {
    std::string_view expression;
    std::string_view expected;
  };
  const std::vector<Case> cases{
      {R"(gisland.ui.text { value = "hello", role = "body", truncation = "middle" })",
       R"({"type":"text","value":"hello","role":"body","truncation":"middle"})"},
      {R"(gisland.ui.icon { name = "bell", accessible_label = "Alert", role = "warning" })",
       R"({"type":"icon","name":"bell","accessible_label":"Alert","role":"warning"})"},
      {R"(gisland.ui.image { resource_id = "cover", role = "thumbnail", accessible_label = "Cover" })",
       R"({"type":"image","resource_id":"cover","role":"thumbnail","accessible_label":"Cover"})"},
      {R"(gisland.ui.rich_text { role = "body", content = {
          { type = "text", value = "Open ", emphasis = { "bold" } },
          { type = "link", value = "details", emphasis = { "underline" },
            action_id = "open", accessible_label = "Open details" },
          { type = "inline_image", resource_id = "dot", role = "status",
            accessible_label = "Online" }
        } })",
       R"({"type":"rich_text","role":"body","content":[{"type":"text","value":"Open ","emphasis":["bold"]},{"type":"link","value":"details","emphasis":["underline"],"action_id":"open","accessible_label":"Open details"},{"type":"inline_image","resource_id":"dot","role":"status","accessible_label":"Online"}]})"},
      {R"(gisland.ui.row { alignment = "start", gap = "small",
          gisland.ui.text { value = "A", role = "body" },
          gisland.ui.icon { name = "check", accessible_label = "Done" }
        })",
       R"({"type":"row","alignment":"start","gap":"small","children":[{"type":"text","value":"A","role":"body"},{"type":"icon","name":"check","accessible_label":"Done"}]})"},
      {R"(gisland.ui.column { alignment = "end", gap = "large",
          gisland.ui.spacer { flexible = false, size_token = "small" }
        })",
       R"({"type":"column","alignment":"end","gap":"large","children":[{"type":"spacer","flexible":false,"size_token":"small"}]})"},
      {R"(gisland.ui.spacer { flexible = false, size_token = "large" })",
       R"({"type":"spacer","flexible":false,"size_token":"large"})"},
      {R"(gisland.ui.progress { value = 0.5, label = "50 percent", state = "foreground",
          shape = "ring", transition_from = 0.4 })",
       R"({"type":"progress","value":0.5,"label":"50 percent","state":"foreground","shape":"ring","transition_from":0.4})"},
      {R"(gisland.ui.indicator { state = "warning", accessible_label = "Needs attention",
          effects = { "shadow", "glow", "breathe" } })",
       R"({"type":"indicator","state":"warning","accessible_label":"Needs attention","effects":["shadow","glow","breathe"]})"},
      {R"(gisland.ui.button { action_id = "accept", enabled = false,
          accessible_label = "Accept", content = gisland.ui.text { value = "OK", role = "button" }
        })",
       R"({"type":"button","action_id":"accept","enabled":false,"accessible_label":"Accept","content":{"type":"text","value":"OK","role":"button"}})"},
      {R"(gisland.ui.action_region { action_id = "open", enabled = true,
          accessible_label = "Open item", content = gisland.ui.icon {
            name = "folder", accessible_label = "Folder"
          } })",
       R"({"type":"action_region","action_id":"open","enabled":true,"accessible_label":"Open item","content":{"type":"icon","name":"folder","accessible_label":"Folder"}})"},
  };

  for (const auto &[expression, expected] : cases) {
    CAPTURE(expression);
    const auto record = emitted_record(expression);
    CHECK(record.at("views").at("compact") == Json::parse(expected));
  }
}

TEST_CASE("lua scene constructors preserve deeply nested child arrays", "[lua_scene]") {
  const auto record = emitted_record(R"lua(
gisland.ui.column { gap = "normal",
  gisland.ui.row {
    gisland.ui.button { action_id = "select", accessible_label = "Select",
      content = gisland.ui.rich_text { role = "body", content = {
        { type = "text", value = "one" },
        { type = "text", value = "two", emphasis = { "italic" } }
      } }
    },
    gisland.ui.spacer {}
  }
}
)lua");

  const auto &root = record.at("views").at("compact");
  REQUIRE(root.at("children").size() == 1);
  REQUIRE(root.at("children").at(0).at("children").size() == 2);
  CHECK(root.at("children").at(0).at("children").at(0).at("content").at("content").size() == 2);
}

TEST_CASE("lua indicator accepts an explicitly empty effects array", "[lua_scene]") {
  const auto record =
      emitted_record("gisland.ui.indicator { state = 'idle', accessible_label = 'Idle', "
                     "effects = gisland.array() }");
  CHECK(record.at("views").at("compact").at("effects") == Json::array());
}

TEST_CASE("lua scene API reports useful paths for malformed fields", "[lua_scene]") {
  const auto check_error = [](std::string_view expression, std::string_view diagnostic) {
    const auto [result, records] = run_script(publishing(expression));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaHostErrorCode::callback_error);
    CHECK(result.error().message.find(diagnostic) != std::string::npos);
    CHECK(records.empty());
  };

  SECTION("missing field") { check_error("gisland.ui.text { role = 'body' }", "ui.text/value"); }
  SECTION("unknown field") {
    check_error("gisland.ui.text { value = 'x', role = 'body', color = '#fff' }", "ui.text/color");
  }
  SECTION("wrong field type") {
    check_error("gisland.ui.progress { value = 'half' }", "ui.progress/value");
  }
  SECTION("malformed positional children") {
    check_error("gisland.ui.row { 'not a node' }", "ui.row/children/0");
  }
  SECTION("sparse children") {
    check_error("gisland.ui.row { [2] = gisland.ui.spacer {} }", "ui.row/children");
  }
  SECTION("malformed rich content") {
    check_error("gisland.ui.rich_text { role = 'body', content = { { type = 'link' } } }",
                "ui.rich_text/content/0/value");
  }
  SECTION("indicator effects must be an array") {
    check_error(
        "gisland.ui.indicator { state = 'warning', accessible_label = 'Warning', effects = {} }",
        "ui.indicator/effects");
  }
  SECTION("indicator effects must contain strings") {
    check_error("gisland.ui.indicator { state = 'warning', accessible_label = 'Warning', "
                "effects = { 1 } }",
                "ui.indicator/effects/0");
  }
  SECTION("indicator effects must be unique") {
    check_error("gisland.ui.indicator { state = 'warning', accessible_label = 'Warning', "
                "effects = { 'glow', 'glow' } }",
                "ui.indicator/effects/1");
  }
  SECTION("indicator effects must be supported") {
    check_error("gisland.ui.indicator { state = 'warning', accessible_label = 'Warning', "
                "effects = { 'pulse' } }",
                "ui.indicator/effects/0");
  }
  SECTION("unknown publication field") {
    const auto [result, records] = run_script(R"lua(return gisland.module { init = function()
      gisland.publish { context_id = "x", priority = 1, color = "red",
        views = { compact = gisland.ui.spacer {} } }
    end })lua");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("publish/color") != std::string::npos);
    CHECK(records.empty());
  }
}

TEST_CASE("lua publish accepts strict content transitions", "[lua_scene]") {
  const auto [result, records] = run_script(R"lua(return gisland.module { init = function()
    gisland.publish {
      context_id = "transition", priority = 1,
      views = {
        compact = gisland.ui.spacer {},
        expanded = gisland.ui.spacer {},
      },
      transitions = { compact = "crossfade", expanded = "slide-right" },
    }
  end })lua",
                                            {"content-transitions"}, 9);

  REQUIRE(result.has_value());
  REQUIRE(records.size() == 2);
  CHECK(records[1].at("transitions") ==
        Json{{"compact", "crossfade"}, {"expanded", "slide-right"}});
}

TEST_CASE("lua publish rejects malformed content transitions", "[lua_scene]") {
  const auto check_error = [](std::string_view transitions, std::string_view diagnostic) {
    const auto script =
        "return gisland.module { init = function() gisland.publish { context_id = 'x', "
        "priority = 1, views = { compact = gisland.ui.spacer {} }, transitions = " +
        std::string{transitions} + " } end }";
    const auto [result, records] = run_script(script, {"content-transitions"}, 9);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find(diagnostic) != std::string::npos);
    CHECK(records.empty());
  };

  check_error("gisland.array()", "publish/transitions");
  check_error("{}", "publish/transitions");
  check_error("{ other = 'crossfade' }", "publish/transitions/other");
  check_error("{ compact = 1 }", "publish/transitions/compact");
  check_error("{ expanded = 'zoom' }", "publish/transitions/expanded");
}

TEST_CASE("lua publish transitions require matching views", "[lua_scene]") {
  const auto check_error = [](std::string_view view, std::string_view transition,
                              std::string_view path) {
    const auto script =
        "return gisland.module { init = function() gisland.publish { context_id = 'x', "
        "priority = 1, views = { " +
        std::string{view} + " = gisland.ui.spacer {} }, transitions = { " +
        std::string{transition} + " = 'crossfade' } } end }";
    const auto [result, records] = run_script(script, {"content-transitions"}, 9);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find(path) != std::string::npos);
    CHECK(records.empty());
  };

  check_error("expanded", "compact", "publish/transitions/compact");
  check_error("compact", "expanded", "publish/transitions/expanded");
}

TEST_CASE("lua scene API enforces local bounds without final scene validation", "[lua_scene]") {
  const auto check_error = [](std::string_view script, std::string_view diagnostic) {
    const auto [result, records] = run_script(script);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find(diagnostic) != std::string::npos);
    CHECK(records.empty());
  };

  check_error(publishing("gisland.ui.progress { value = 1.01 }"), "ui.progress/value");
  check_error(publishing("gisland.ui.progress { value = 0.5, transition_from = -0.1 }"),
              "ui.progress/transition_from");
  check_error(R"lua(return gisland.module { init = function()
    gisland.publish { context_id = "", priority = 1,
      views = { compact = gisland.ui.spacer {} } }
  end })lua",
              "publish/context_id");
  check_error(R"lua(return gisland.module { init = function()
    gisland.publish { context_id = "x", priority = 1, expires_in_ms = -1,
      views = { compact = gisland.ui.spacer {} } }
  end })lua",
              "publish/expires_in_ms");
  check_error(R"lua(return gisland.module { init = function()
    gisland.publish { context_id = "x", priority = 1,
      views = { compact = gisland.ui.spacer {}, expanded = gisland.ui.spacer {} },
      presentation = { reveal = "expanded", duration_ms = 60001 } }
  end })lua",
              "publish/presentation/duration_ms");
  check_error(R"lua(return gisland.module { init = function()
    gisland.publish { context_id = "x", priority = 1,
      views = { compact = gisland.ui.spacer {} },
      resources = { { id = "x", format = "rgba8", width = 513, height = 1, data = "" } } }
  end })lua",
              "publish/resources/0/width");
  check_error(R"lua(return gisland.module { init = function()
    gisland.log("verbose", "x")
  end })lua",
              "log/level");
}

TEST_CASE("lua scene accepts protocol-sized image data but still bounds ordinary text",
          "[lua_scene]") {
  const std::string resource_data(64 * 1024, 'A');
  const auto resource_script =
      "local data = string.rep('A', 65536)\nreturn gisland.module { init = function() "
      "gisland.publish { context_id = 'x', priority = 1, views = { compact = "
      "gisland.ui.image { resource_id = 'x', role = 'image', accessible_label = 'Image' } }, "
      "resources = { "
      "{ id = 'x', format = 'rgba8', width = 1, height = 1, data = data } } } end }";
  const auto [resource_result, resource_records] = run_script(resource_script);
  REQUIRE(resource_result.has_value());
  REQUIRE(resource_records.size() == 2);
  CHECK(resource_records[1].at("resources")[0].at("data") == resource_data);

  const auto [text_result, text_records] =
      run_script("local text = string.rep('x', 4097)\nreturn gisland.module { init = function() "
                 "gisland.publish { context_id = 'x', priority = 1, views = { compact = "
                 "gisland.ui.text { value = text, role = 'body' } } } end }");
  REQUIRE_FALSE(text_result.has_value());
  CHECK(text_result.error().message.find("ui.text/value") != std::string::npos);
  CHECK(text_records.empty());
}

TEST_CASE("lua scene API rejects concrete styling but preserves semantic roles", "[lua_scene]") {
  const auto semantic = emitted_record(
      "gisland.ui.icon { name = 'volume', accessible_label = 'Volume', role = 'hud-volume-icon' }");
  CHECK(semantic.at("views").at("compact").at("role") == "hud-volume-icon");

  const auto [result, records] =
      run_script(publishing("gisland.ui.text { value = 'x', role = 'warning', font_size = 24 }"));
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().message.find("ui.text/font_size") != std::string::npos);
  CHECK(records.empty());
}
