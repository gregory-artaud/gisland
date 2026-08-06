#include "gisland/x11_monitor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

namespace {

constexpr std::array monitors{
    gisland::X11Monitor{"DP-1", -1920, 0, 1920, 1080, false},
    gisland::X11Monitor{"HDMI-1", 0, 0, 2560, 1440, true},
};

} // namespace

TEST_CASE("monitor selection resolves an exact active output") {
  const auto selected = gisland::select_monitor(monitors, "DP-1");

  REQUIRE(selected.has_value());
  CHECK(selected->monitor.name == "DP-1");
  CHECK(selected->monitor.x == -1920);
  CHECK_FALSE(selected->used_fallback);
}

TEST_CASE("primary and missing names select the active primary output") {
  const auto primary = gisland::select_monitor(monitors, "primary");
  REQUIRE(primary.has_value());
  CHECK(primary->monitor.name == "HDMI-1");
  CHECK_FALSE(primary->used_fallback);

  const auto missing = gisland::select_monitor(monitors, "DP-9");
  REQUIRE(missing.has_value());
  CHECK(missing->monitor.name == "HDMI-1");
  CHECK(missing->used_fallback);
}

TEST_CASE("first active output is the final deterministic fallback") {
  const std::array no_primary{
      gisland::X11Monitor{"DP-2", 20, 30, 1280, 720, false},
      gisland::X11Monitor{"DP-3", 1300, 30, 1280, 720, false},
  };

  const auto selected = gisland::select_monitor(no_primary, "missing");

  REQUIRE(selected.has_value());
  CHECK(selected->monitor.name == "DP-2");
  CHECK(selected->used_fallback);
}

TEST_CASE("monitor selection rejects empty or invalid active topology") {
  const auto empty = gisland::select_monitor({}, "primary");
  REQUIRE_FALSE(empty.has_value());
  CHECK(empty.error().code == gisland::MonitorErrorCode::no_active_outputs);

  const std::array invalid{
      gisland::X11Monitor{"DP-1", 0, 0, 0, 1080, true},
      gisland::X11Monitor{"DP-2", 0, 0, 1920, -1, false},
  };
  const auto invalid_result = gisland::select_monitor(invalid, "primary");
  REQUIRE_FALSE(invalid_result.has_value());
  CHECK(invalid_result.error().code == gisland::MonitorErrorCode::no_active_outputs);
}

TEST_CASE("monitor selection returns to a named output when it reappears") {
  const std::array fallback_only{
      gisland::X11Monitor{"HDMI-1", 0, 0, 1920, 1080, true},
  };
  const auto fallback = gisland::select_monitor(fallback_only, "DP-1");
  REQUIRE(fallback.has_value());
  CHECK(fallback->used_fallback);

  const auto recovered = gisland::select_monitor(monitors, "DP-1");
  REQUIRE(recovered.has_value());
  CHECK(recovered->monitor.name == "DP-1");
  CHECK_FALSE(recovered->used_fallback);
}

TEST_CASE("window placement remains centered on outputs with signed origins") {
  const auto placement = gisland::place_on_monitor(monitors.front(), 420, 220, 8);

  REQUIRE(placement.has_value());
  CHECK(placement->x == -1170);
  CHECK(placement->y == 8);
}

TEST_CASE("window placement compensates shadow canvas margins") {
  const gisland::X11Monitor monitor{"DP-1", 100, 50, 1000, 800, true};
  const gisland::X11CanvasGeometry canvas{140, 80, 16, 14, 100};

  const auto placement = gisland::place_on_monitor(monitor, canvas, 10);

  REQUIRE(placement.has_value());
  CHECK(*placement == gisland::X11WindowPlacement{534, 46});
  CHECK(placement->x + canvas.surface_x + (canvas.surface_width / 2) == 600);
  CHECK(placement->y + canvas.surface_y == 60);
}

TEST_CASE("window placement rejects invalid dimensions and arithmetic overflow") {
  CHECK_FALSE(gisland::place_on_monitor(monitors.front(), 0, 220, 8).has_value());
  const gisland::X11Monitor extreme{
      "DP-1", std::numeric_limits<int>::max(), 0, std::numeric_limits<int>::max(), 1080, true};
  CHECK_FALSE(gisland::place_on_monitor(extreme, 1, 1, 8).has_value());
}
