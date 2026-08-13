#include "gisland/display.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

namespace {

constexpr std::array outputs{
    gisland::DisplayOutput{"DP-1", -1920, 0, 1920, 1080, false},
    gisland::DisplayOutput{"HDMI-1", 0, 0, 2560, 1440, true},
};

} // namespace

TEST_CASE("output selection resolves an exact active output") {
  const auto selected = gisland::select_output(outputs, "DP-1");

  REQUIRE(selected.has_value());
  CHECK(selected->output.name == "DP-1");
  CHECK(selected->output.x == -1920);
  CHECK_FALSE(selected->used_fallback);
}

TEST_CASE("primary and missing names select the preferred output") {
  const auto preferred = gisland::select_output(outputs, "primary");
  REQUIRE(preferred.has_value());
  CHECK(preferred->output.name == "HDMI-1");
  CHECK_FALSE(preferred->used_fallback);

  const auto missing = gisland::select_output(outputs, "DP-9");
  REQUIRE(missing.has_value());
  CHECK(missing->output.name == "HDMI-1");
  CHECK(missing->used_fallback);
}

TEST_CASE("first active output is the final deterministic fallback") {
  const std::array no_preferred{
      gisland::DisplayOutput{"DP-2", 20, 30, 1280, 720, false},
      gisland::DisplayOutput{"DP-3", 1300, 30, 1280, 720, false},
  };

  const auto selected = gisland::select_output(no_preferred, "missing");

  REQUIRE(selected.has_value());
  CHECK(selected->output.name == "DP-2");
  CHECK(selected->used_fallback);
}

TEST_CASE("output selection rejects empty or invalid active topology") {
  const auto empty = gisland::select_output({}, "primary");
  REQUIRE_FALSE(empty.has_value());
  CHECK(empty.error().code == gisland::DisplayErrorCode::no_active_outputs);
  CHECK(empty.error().message == "display has no active outputs");

  const std::array invalid{
      gisland::DisplayOutput{"DP-1", 0, 0, 0, 1080, true},
      gisland::DisplayOutput{"DP-2", 0, 0, 1920, -1, false},
  };
  const auto invalid_result = gisland::select_output(invalid, "primary");
  REQUIRE_FALSE(invalid_result.has_value());
  CHECK(invalid_result.error().code == gisland::DisplayErrorCode::no_active_outputs);
}

TEST_CASE("output selection returns to a named output when it reappears") {
  const std::array fallback_only{
      gisland::DisplayOutput{"HDMI-1", 0, 0, 1920, 1080, true},
  };
  const auto fallback = gisland::select_output(fallback_only, "DP-1");
  REQUIRE(fallback.has_value());
  CHECK(fallback->used_fallback);

  const auto recovered = gisland::select_output(outputs, "DP-1");
  REQUIRE(recovered.has_value());
  CHECK(recovered->output.name == "DP-1");
  CHECK_FALSE(recovered->used_fallback);
}

TEST_CASE("absolute placement remains centered on outputs with signed origins") {
  const auto placement = gisland::place_canvas(outputs.front(), 420, 220, 8);

  REQUIRE(placement.has_value());
  CHECK(placement->x == -1170);
  CHECK(placement->y == 8);
}

TEST_CASE("absolute placement centers the visible surface and compensates canvas margins") {
  const gisland::DisplayOutput output{"DP-1", 100, 50, 1000, 800, true};
  const gisland::CanvasGeometry canvas{140, 80, 16, 14, 100};

  const auto placement = gisland::place_canvas(output, canvas, 10);

  REQUIRE(placement.has_value());
  CHECK(*placement == gisland::AbsolutePlacement{534, 46});
  CHECK(placement->x + canvas.surface_x + (canvas.surface_width / 2) == 600);
  CHECK(placement->y + canvas.surface_y == 60);
}

TEST_CASE("absolute placement rejects invalid dimensions bounds and arithmetic overflow") {
  CHECK_FALSE(gisland::place_canvas(outputs.front(), 0, 220, 8).has_value());
  CHECK_FALSE(gisland::place_canvas(outputs.front(), gisland::CanvasGeometry{100, 80, 90, 0, 20}, 8)
                  .has_value());
  const gisland::DisplayOutput extreme{
      "DP-1", std::numeric_limits<int>::max(), 0, std::numeric_limits<int>::max(), 1080, true};
  CHECK_FALSE(gisland::place_canvas(extreme, 1, 1, 8).has_value());
}
