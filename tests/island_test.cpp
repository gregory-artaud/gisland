#include "gisland/island.hpp"
#include "gisland/x11_shape.hpp"

#include <X11/Xlib.h>
#include <X11/extensions/shape.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <limits>

using Catch::Approx;

TEST_CASE("island modes have stable geometry") {
  const auto compact = gisland::geometry_for(gisland::IslandMode::compact);
  CHECK(compact.width == Approx(220.0F));
  CHECK(compact.height == Approx(44.0F));
  CHECK(compact.radius == Approx(22.0F));

  const auto expanded = gisland::geometry_for(gisland::IslandMode::expanded);
  CHECK(expanded.width == Approx(420.0F));
  CHECK(expanded.height == Approx(220.0F));
  CHECK(expanded.radius == Approx(24.0F));

  const auto halfway = gisland::interpolate(compact, expanded, 0.5F);
  CHECK(halfway.width == Approx(320.0F));
  CHECK(halfway.height == Approx(132.0F));
  CHECK(halfway.radius == Approx(23.0F));
}

TEST_CASE("island stays centered at the top of its fixed canvas") {
  const auto canvas = gisland::island_canvas_size();
  CHECK(canvas.width == Approx(440.0F));
  CHECK(canvas.height == Approx(232.0F));

  const auto placement =
      gisland::place_at_top_center(gisland::geometry_for(gisland::IslandMode::compact), canvas);
  CHECK(placement.x == Approx(110.0F));
  CHECK(placement.y == Approx(0.0F));
}

TEST_CASE("spring motion has subtle overshoot and preserves reversal velocity") {
  gisland::SpringProgress spring;
  spring.set_target(1.0F);
  spring.update(1.0F / 60.0F);
  CHECK(spring.value() > 0.0F);
  CHECK(spring.value() < 1.0F);

  float maximum_progress = spring.value();
  for (int frame = 0; frame < 60; ++frame) {
    spring.update(1.0F / 60.0F);
    maximum_progress = std::max(maximum_progress, spring.value());
  }
  CHECK(maximum_progress > 1.0F);
  CHECK(maximum_progress < 1.08F);
  CHECK(spring.value() == Approx(1.0F).margin(0.001F));

  gisland::SpringProgress interrupted;
  interrupted.set_target(1.0F);
  interrupted.update(0.05F);
  const float progress_before_reversal = interrupted.value();
  interrupted.set_target(0.0F);
  interrupted.update(0.001F);
  CHECK(interrupted.value() > progress_before_reversal);
}

TEST_CASE("hover expands immediately and collapses after its exit tolerance") {
  gisland::HoverController hover;
  hover.update(true, 0.0F);
  CHECK(hover.mode() == gisland::IslandMode::expanded);
  hover.update(false, 0.149F);
  CHECK(hover.mode() == gisland::IslandMode::expanded);
  hover.update(false, 0.001F);
  CHECK(hover.mode() == gisland::IslandMode::compact);
}

TEST_CASE("content crossfade starts with only compact content visible") {
  const gisland::ContentCrossfade crossfade;

  const auto compact = crossfade.compact();
  CHECK(compact.opacity == Approx(1.0F));
  CHECK(compact.blur == Approx(0.0F));
  CHECK(compact.scale == Approx(1.0F));

  const auto expanded = crossfade.expanded();
  CHECK(expanded.opacity == Approx(0.0F));
  CHECK(expanded.blur == Approx(6.0F));
  CHECK(expanded.scale == Approx(0.96F));
}

TEST_CASE("content crossfade delays the incoming layer while outgoing content leaves") {
  gisland::ContentCrossfade crossfade;
  crossfade.set_mode(gisland::IslandMode::expanded);
  crossfade.update(0.05F);

  const auto outgoing = crossfade.compact();
  CHECK(outgoing.opacity < 1.0F);
  CHECK(outgoing.blur > 0.0F);
  CHECK(outgoing.scale < 1.0F);

  const auto delayed = crossfade.expanded();
  CHECK(delayed.opacity == Approx(0.0F));
  CHECK(delayed.blur == Approx(6.0F));
  CHECK(delayed.scale == Approx(0.96F));

  crossfade.update(0.02F);
  const auto incoming = crossfade.expanded();
  CHECK(incoming.opacity > 0.0F);
  CHECK(incoming.blur < 6.0F);
  CHECK(incoming.scale > 0.96F);
}

TEST_CASE("content crossfade settles and reverses from its current values") {
  gisland::ContentCrossfade crossfade;
  crossfade.set_mode(gisland::IslandMode::expanded);
  crossfade.update(0.41F);

  CHECK(crossfade.compact().opacity == Approx(0.0F));
  CHECK(crossfade.expanded().opacity == Approx(1.0F));
  CHECK(crossfade.expanded().blur == Approx(0.0F));
  CHECK(crossfade.expanded().scale == Approx(1.0F));

  crossfade.set_mode(gisland::IslandMode::compact);
  const auto compact_before_delay = crossfade.compact();
  crossfade.update(0.03F);
  CHECK(crossfade.compact().opacity == Approx(compact_before_delay.opacity));
  CHECK(crossfade.expanded().opacity < 1.0F);

  crossfade.update(0.38F);
  CHECK(crossfade.compact().opacity == Approx(1.0F));
  CHECK(crossfade.expanded().opacity == Approx(0.0F));
}

TEST_CASE("content crossfade preserves continuity when reversed mid-flight") {
  gisland::ContentCrossfade crossfade;
  crossfade.set_mode(gisland::IslandMode::expanded);
  crossfade.update(0.15F);

  const auto compact_before_reversal = crossfade.compact();
  const auto expanded_before_reversal = crossfade.expanded();
  crossfade.set_mode(gisland::IslandMode::compact);

  CHECK(crossfade.compact().opacity == Approx(compact_before_reversal.opacity));
  CHECK(crossfade.compact().blur == Approx(compact_before_reversal.blur));
  CHECK(crossfade.compact().scale == Approx(compact_before_reversal.scale));
  CHECK(crossfade.expanded().opacity == Approx(expanded_before_reversal.opacity));
  CHECK(crossfade.expanded().blur == Approx(expanded_before_reversal.blur));
  CHECK(crossfade.expanded().scale == Approx(expanded_before_reversal.scale));

  crossfade.update(0.03F);
  CHECK(crossfade.compact().opacity == Approx(compact_before_reversal.opacity));
  CHECK(crossfade.expanded().opacity < expanded_before_reversal.opacity);
}

TEST_CASE("rounded mask covers the middle and insets its edges") {
  const auto mask = gisland::rounded_mask_rows(gisland::geometry_for(gisland::IslandMode::compact));
  REQUIRE(mask.size() == 44);
  CHECK(mask.front().x > 0);
  CHECK(mask.at(22).x == 0);
  CHECK(mask.back().x > 0);
}

TEST_CASE("input shape does not clip antialiased rendering") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  Display *display = XOpenDisplay(nullptr);
  REQUIRE(display != nullptr);

  const auto geometry = gisland::geometry_for(gisland::IslandMode::compact);
  const auto canvas = gisland::island_canvas_size();
  const auto placement = gisland::place_at_top_center(geometry, canvas);
  Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0,
                                      static_cast<unsigned int>(canvas.width),
                                      static_cast<unsigned int>(canvas.height), 0, 0, 0);
  XSync(display, False);

  gisland::RoundedWindowShape shape;
  shape.apply(&window, geometry, placement);
  XSync(display, False);

  Bool bounding_shaped = False;
  Bool clip_shaped = False;
  int x = 0;
  int y = 0;
  unsigned int width = 0;
  unsigned int height = 0;
  XShapeQueryExtents(display, window, &bounding_shaped, &x, &y, &width, &height, &clip_shaped, &x,
                     &y, &width, &height);

  int rectangle_count = 0;
  int ordering = 0;
  XRectangle *input_rectangles =
      XShapeGetRectangles(display, window, ShapeInput, &rectangle_count, &ordering);
  int minimum_x = std::numeric_limits<int>::max();
  for (int index = 0; index < rectangle_count; ++index) {
    minimum_x = std::min(minimum_x, static_cast<int>(input_rectangles[index].x));
  }

  CHECK_FALSE(bounding_shaped);
  CHECK_FALSE(clip_shaped);
  CHECK(rectangle_count > 1);
  CHECK(minimum_x == static_cast<int>(placement.x));

  XFree(input_rectangles);
  XDestroyWindow(display, window);
  XCloseDisplay(display);
}
