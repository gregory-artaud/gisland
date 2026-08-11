#include "gisland/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_CASE("a row containing every primitive is a valid scene") {
  std::vector<gisland::SceneNode> children;
  children.emplace_back(gisland::Text{"14:32", "body"});
  children.emplace_back(gisland::Icon{"clock", "Current time"});
  children.emplace_back(gisland::Image{"cover", "notification-icon", "Album cover"});
  children.emplace_back(gisland::Spacer{true, ""});
  children.emplace_back(gisland::Progress{0.5, "Half complete", "accent"});
  children.emplace_back(gisland::Indicator{"success", "Available"});
  children.emplace_back(gisland::Button{gisland::SceneNode{gisland::Text{"Open", "label"}}, "open",
                                        true, "Open details"});
  children.emplace_back(gisland::Column{{gisland::SceneNode{gisland::Text{"Today", "caption"}}}});

  const gisland::SceneNode scene{gisland::Row{std::move(children)}};

  CHECK(gisland::validate_scene(scene).has_value());
}

TEST_CASE("scene depth is bounded") {
  gisland::SceneNode maximum_depth{gisland::Text{"x", "body"}};
  for (int depth = 1; depth < 16; ++depth) {
    maximum_depth = gisland::SceneNode{gisland::Column{{std::move(maximum_depth)}}};
  }
  CHECK(gisland::validate_scene(maximum_depth).has_value());

  gisland::SceneNode too_deep{gisland::Column{{std::move(maximum_depth)}}};
  const auto result = gisland::validate_scene(too_deep);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == gisland::SceneErrorCode::too_deep);
}

TEST_CASE("scene node count is bounded") {
  std::vector<gisland::SceneNode> maximum_children;
  maximum_children.reserve(255);
  for (int index = 0; index < 255; ++index) {
    maximum_children.emplace_back(gisland::Spacer{});
  }
  const gisland::SceneNode maximum_scene{gisland::Row{std::move(maximum_children)}};
  CHECK(gisland::validate_scene(maximum_scene).has_value());

  std::vector<gisland::SceneNode> too_many_children;
  too_many_children.reserve(256);
  for (int index = 0; index < 256; ++index) {
    too_many_children.emplace_back(gisland::Spacer{});
  }
  const auto result =
      gisland::validate_scene(gisland::SceneNode{gisland::Row{std::move(too_many_children)}});

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == gisland::SceneErrorCode::too_many_nodes);
}

TEST_CASE("text values are bounded by UTF-8 byte count") {
  CHECK(gisland::validate_scene(gisland::SceneNode{gisland::Text{std::string(4096, 'x'), "body"}})
            .has_value());

  const auto result =
      gisland::validate_scene(gisland::SceneNode{gisland::Text{std::string(4097, 'x'), "body"}});

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == gisland::SceneErrorCode::text_too_long);
}

TEST_CASE("progress values must be finite and normalized") {
  for (const double value : {-0.01, 1.01, std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
    const auto result =
        gisland::validate_scene(gisland::SceneNode{gisland::Progress{value, "", "accent"}});

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::SceneErrorCode::invalid_progress);
  }
}

TEST_CASE("button action IDs must not be empty") {
  const auto result = gisland::validate_scene(gisland::SceneNode{
      gisland::Button{gisland::SceneNode{gisland::Text{"Dismiss", "label"}}, "", true, "Dismiss"}});

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == gisland::SceneErrorCode::empty_action);
}

TEST_CASE("scene display strings are bounded by UTF-8 byte count") {
  const std::string oversized(4097, 'x');
  const auto check = [](gisland::SceneNode scene, std::string_view path) {
    const auto result = gisland::validate_scene(scene);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::SceneErrorCode::text_too_long);
    CHECK(result.error().path == path);
  };

  check(gisland::SceneNode{gisland::Icon{"clock", oversized}}, "/accessible_label");
  check(gisland::SceneNode{gisland::Image{"cover", "notification-icon", oversized}},
        "/accessible_label");
  check(gisland::SceneNode{gisland::Progress{0.5, oversized, "accent"}}, "/label");
  check(gisland::SceneNode{gisland::Indicator{"success", oversized}}, "/accessible_label");
  check(gisland::SceneNode{gisland::Button{gisland::SceneNode{gisland::Text{"x", "body"}}, "open",
                                           true, oversized}},
        "/accessible_label");
}

TEST_CASE("scene semantic strings are bounded with exact paths") {
  const std::string oversized(129, 'x');
  const auto check = [](gisland::SceneNode scene, std::string_view path) {
    const auto result = gisland::validate_scene(scene);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::SceneErrorCode::identifier_too_long);
    CHECK(result.error().path == path);
  };

  check(gisland::SceneNode{gisland::Text{"x", oversized}}, "/role");
  check(gisland::SceneNode{gisland::Text{"x", "body", oversized}}, "/truncation");
  check(gisland::SceneNode{gisland::Icon{oversized, "Clock"}}, "/name");
  check(gisland::SceneNode{gisland::Image{oversized, "notification-icon", "Cover"}},
        "/resource_id");
  check(gisland::SceneNode{gisland::Image{"cover", oversized, "Cover"}}, "/role");
  check(gisland::SceneNode{gisland::Spacer{false, oversized}}, "/size_token");
  check(gisland::SceneNode{gisland::Progress{0.5, "Half", oversized}}, "/state");
  check(gisland::SceneNode{gisland::Indicator{oversized, "Available"}}, "/state");
  check(gisland::SceneNode{gisland::Row{{}, oversized, "normal"}}, "/alignment");
  check(gisland::SceneNode{gisland::Row{{}, "center", oversized}}, "/gap");
  check(gisland::SceneNode{gisland::Column{{}, oversized, "normal"}}, "/alignment");
  check(gisland::SceneNode{gisland::Column{{}, "center", oversized}}, "/gap");
  check(gisland::SceneNode{gisland::Button{gisland::SceneNode{gisland::Text{"x", "body"}},
                                           oversized}},
        "/action_id");
}

TEST_CASE("rich text and invisible action regions are valid scene values") {
  gisland::RichText rich{
      "notification-body",
      {
          gisland::RichTextSpan{"The file ", {}},
          gisland::RichTextSpan{"archive.tar.gz", {gisland::TextEmphasis::bold}},
          gisland::RichLinkSpan{
              "Open folder", {gisland::TextEmphasis::underline}, "link-0", "Open download folder"},
          gisland::RichInlineImage{"preview", "notification-inline-image", "File preview"},
      },
  };
  const gisland::SceneNode scene{gisland::ActionRegion{gisland::SceneNode{std::move(rich)},
                                                       "default", true, "Open notification"}};

  CHECK(gisland::validate_scene(scene).has_value());
}

TEST_CASE("rich text bounds content and actions with exact paths") {
  SECTION("aggregate text") {
    gisland::RichText rich{"notification-body",
                           {gisland::RichTextSpan{std::string(4096, 'x'), {}},
                            gisland::RichTextSpan{std::string(4096, 'x'), {}},
                            gisland::RichTextSpan{std::string(4096, 'x'), {}},
                            gisland::RichTextSpan{std::string(4096, 'x'), {}},
                            gisland::RichTextSpan{"x", {}}}};
    const auto result = gisland::validate_scene(gisland::SceneNode{std::move(rich)});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::SceneErrorCode::text_too_long);
    CHECK(result.error().path == "/content/4/value");
  }

  SECTION("link action") {
    gisland::RichText rich{
        "notification-body",
        {gisland::RichLinkSpan{"Open", {}, "", "Open destination"}},
    };
    const auto result = gisland::validate_scene(gisland::SceneNode{std::move(rich)});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::SceneErrorCode::empty_action);
    CHECK(result.error().path == "/content/0/action_id");
  }

  SECTION("action region") {
    const auto result = gisland::validate_scene(gisland::SceneNode{gisland::ActionRegion{
        gisland::SceneNode{gisland::Text{"Open", "body"}}, "", true, "Open"}});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::SceneErrorCode::empty_action);
    CHECK(result.error().path == "/action_id");
  }
}
