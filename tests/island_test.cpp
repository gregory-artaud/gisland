#include "gisland/island.hpp"
#include "gisland/x11_shape.hpp"

#include <X11/Xlib.h>
#include <X11/extensions/shape.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>

using Catch::Approx;
using namespace std::chrono_literals;

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

TEST_CASE("shadow-aware canvas placement preserves its surface anchor") {
  const gisland::IslandCanvasSize canvas{
      .width = 140.0F,
      .height = 80.0F,
      .surface_x = 16.0F,
      .surface_y = 14.0F,
      .surface_width = 100.0F,
      .surface_height = 40.0F,
  };

  CHECK(gisland::place_at_top_center({80.0F, 32.0F, 16.0F}, canvas) ==
        gisland::IslandPlacement{26.0F, 14.0F});
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
  hover.update(false, 0.119F);
  CHECK(hover.mode() == gisland::IslandMode::expanded);
  hover.update(false, 0.001F);
  CHECK(hover.mode() == gisland::IslandMode::compact);
}

TEST_CASE("hover exit tolerance is configurable and permits immediate collapse") {
  gisland::HoverController delayed{450ms};
  delayed.update(true, 0.0F);
  delayed.update(false, 0.449F);
  CHECK(delayed.mode() == gisland::IslandMode::expanded);
  delayed.update(false, 0.001F);
  CHECK(delayed.mode() == gisland::IslandMode::compact);

  gisland::HoverController immediate{0ms};
  immediate.update(true, 0.0F);
  immediate.update(false, 0.0F);
  CHECK(immediate.mode() == gisland::IslandMode::compact);
}

TEST_CASE("hover re-entry cancels collapse and preserves animation continuity") {
  gisland::HoverController hover;
  gisland::SpringProgress spring;
  gisland::ContentCrossfade crossfade;
  hover.update(true, 0.0F);
  spring.set_target(1.0F);
  crossfade.set_mode(hover.mode());
  spring.update(0.05F);
  crossfade.update(0.15F);
  const float spring_before = spring.value();
  const auto compact_before = crossfade.compact();

  hover.update(false, 0.10F);
  CHECK(hover.mode() == gisland::IslandMode::expanded);
  hover.update(true, 0.0F);
  CHECK(hover.mode() == gisland::IslandMode::expanded);

  hover.update(false, 0.119F);
  CHECK(hover.mode() == gisland::IslandMode::expanded);
  hover.update(false, 0.001F);
  REQUIRE(hover.mode() == gisland::IslandMode::compact);
  spring.set_target(0.0F);
  crossfade.set_mode(hover.mode());

  CHECK(spring.value() == Approx(spring_before));
  CHECK(crossfade.compact().opacity == Approx(compact_before.opacity));
  spring.update(0.001F);
  CHECK(spring.value() > spring_before);
}

TEST_CASE("overlay mode combines hover and an explicit open latch") {
  gisland::OverlayModeController controller{120ms};
  controller.update(false, true, 0.0F);
  CHECK(controller.mode() == gisland::IslandMode::compact);

  REQUIRE(controller.open(true).has_value());
  CHECK(controller.mode() == gisland::IslandMode::expanded);
  controller.update(false, true, 1.0F);
  CHECK(controller.mode() == gisland::IslandMode::expanded);

  controller.close();
  CHECK(controller.mode() == gisland::IslandMode::compact);
}

TEST_CASE("explicit close suppresses hover until the pointer exits") {
  gisland::OverlayModeController controller{120ms};
  controller.update(true, true, 0.0F);
  REQUIRE(controller.mode() == gisland::IslandMode::expanded);

  controller.close();
  controller.update(true, true, 1.0F);
  CHECK(controller.mode() == gisland::IslandMode::compact);
  controller.update(false, true, 0.0F);
  CHECK(controller.mode() == gisland::IslandMode::compact);
  controller.update(true, true, 0.0F);
  CHECK(controller.mode() == gisland::IslandMode::expanded);

  controller.close();
  REQUIRE(controller.open(true).has_value());
  controller.update(true, true, 0.0F);
  CHECK(controller.mode() == gisland::IslandMode::expanded);
}

TEST_CASE("overlay toggle uses target mode and requires expanded content") {
  gisland::OverlayModeController controller{120ms};
  REQUIRE(controller.toggle(true).has_value());
  CHECK(controller.mode() == gisland::IslandMode::expanded);
  REQUIRE(controller.toggle(true).has_value());
  CHECK(controller.mode() == gisland::IslandMode::compact);

  const auto unavailable = controller.open(false);
  REQUIRE_FALSE(unavailable.has_value());
  CHECK(unavailable.error() == gisland::ModeControlError::unavailable_expanded);
  CHECK(controller.mode() == gisland::IslandMode::compact);
}

TEST_CASE("overlay mode preserves explicit open only across expandable contexts") {
  gisland::OverlayModeController controller{120ms};
  REQUIRE(controller.open(true).has_value());
  controller.update(false, true, 0.0F);
  CHECK(controller.mode() == gisland::IslandMode::expanded);

  controller.update(false, false, 0.0F);
  CHECK(controller.mode() == gisland::IslandMode::compact);
  controller.update(false, true, 0.0F);
  CHECK(controller.mode() == gisland::IslandMode::compact);
}

TEST_CASE("overlay preview expands for its exact duration") {
  gisland::OverlayModeController controller{120ms};
  controller.start_preview(true, 1000ms);

  CHECK(controller.mode() == gisland::IslandMode::expanded);
  controller.update(false, true, 0.999F);
  CHECK(controller.mode() == gisland::IslandMode::expanded);
  controller.update(false, true, 0.001F);
  CHECK(controller.mode() == gisland::IslandMode::compact);
}

TEST_CASE("overlay preview expiry preserves hover behavior") {
  gisland::OverlayModeController controller{120ms};
  controller.start_preview(true, 1000ms);
  controller.update(true, true, 1.0F);

  CHECK(controller.mode() == gisland::IslandMode::expanded);
  controller.update(false, true, 0.119F);
  CHECK(controller.mode() == gisland::IslandMode::expanded);
  controller.update(false, true, 0.001F);
  CHECK(controller.mode() == gisland::IslandMode::compact);
}

TEST_CASE("explicit open outlives an overlay preview") {
  gisland::OverlayModeController controller{120ms};
  controller.start_preview(true, 1000ms);
  REQUIRE(controller.open(true).has_value());
  controller.update(false, true, 2.0F);

  CHECK(controller.mode() == gisland::IslandMode::expanded);
}

TEST_CASE("toggle closes an active overlay preview") {
  gisland::OverlayModeController controller{120ms};
  controller.start_preview(true, 1000ms);

  REQUIRE(controller.toggle(true).has_value());
  CHECK(controller.mode() == gisland::IslandMode::compact);
}

TEST_CASE("close and unavailable content cancel an overlay preview") {
  gisland::OverlayModeController controller{120ms};
  controller.start_preview(true, 1000ms);
  controller.update(true, true, 0.1F);
  controller.close();
  controller.update(true, true, 1.0F);
  CHECK(controller.mode() == gisland::IslandMode::compact);

  controller.update(false, true, 0.0F);
  controller.start_preview(true, 1000ms);
  controller.update(false, false, 0.0F);
  CHECK(controller.mode() == gisland::IslandMode::compact);
}

TEST_CASE("a new overlay preview replaces the previous duration") {
  gisland::OverlayModeController controller{120ms};
  controller.start_preview(true, 500ms);
  controller.update(false, true, 0.4F);
  controller.start_preview(true, 1000ms);
  controller.update(false, true, 0.999F);
  CHECK(controller.mode() == gisland::IslandMode::expanded);
  controller.update(false, true, 0.001F);
  CHECK(controller.mode() == gisland::IslandMode::compact);
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

TEST_CASE("context transition interpolates geometry and crossfades content") {
  gisland::ContextTransition transition;
  const gisland::IslandGeometry compact{230.0F, 32.0F, 16.0F};
  const gisland::IslandGeometry notification{340.0F, 32.0F, 16.0F};

  CHECK_FALSE(transition.active());
  transition.start(compact, notification, 250ms, gisland::Easing::linear);

  REQUIRE(transition.active());
  auto visual = transition.visual();
  CHECK(visual.geometry.width == Approx(230.0F));
  CHECK(visual.outgoing_opacity == Approx(1.0F));
  CHECK(visual.incoming_opacity == Approx(0.0F));

  transition.update(0.125F);
  visual = transition.visual();
  CHECK(visual.geometry.width == Approx(285.0F));
  CHECK(visual.geometry.height == Approx(32.0F));
  CHECK(visual.outgoing_opacity == Approx(0.5F));
  CHECK(visual.incoming_opacity == Approx(0.5F));

  transition.update(0.125F);
  visual = transition.visual();
  CHECK_FALSE(transition.active());
  CHECK(visual.geometry.width == Approx(340.0F));
  CHECK(visual.outgoing_opacity == Approx(0.0F));
  CHECK(visual.incoming_opacity == Approx(1.0F));
}

TEST_CASE("context transition handles zero duration and bounded frame deltas") {
  gisland::ContextTransition transition;
  const gisland::IslandGeometry source{230.0F, 32.0F, 16.0F};
  const gisland::IslandGeometry target{432.0F, 180.0F, 30.0F};

  transition.start(source, target, 250ms, gisland::Easing::linear);
  transition.update(-1.0F);
  CHECK(transition.visual().geometry == source);

  transition.update(10.0F);
  CHECK_FALSE(transition.active());
  CHECK(transition.visual().geometry == target);

  transition.start(source, target, 0ms, gisland::Easing::ease_in_out);
  CHECK_FALSE(transition.active());
  CHECK(transition.visual().geometry == target);
  CHECK(transition.visual().outgoing_opacity == Approx(0.0F));
  CHECK(transition.visual().incoming_opacity == Approx(1.0F));
}

TEST_CASE("context transition retargets from the visible geometry") {
  gisland::ContextTransition transition;
  const gisland::IslandGeometry clock{230.0F, 32.0F, 16.0F};
  const gisland::IslandGeometry first{340.0F, 32.0F, 16.0F};
  const gisland::IslandGeometry second{432.0F, 160.0F, 30.0F};

  transition.start(clock, first, 250ms, gisland::Easing::linear);
  transition.update(0.1F);
  const auto visible = transition.visual().geometry;

  transition.start(visible, second, 250ms, gisland::Easing::linear);
  CHECK(transition.visual().geometry == visible);
  transition.update(0.125F);
  CHECK(transition.visual().geometry.width == Approx((visible.width + second.width) / 2.0F));
  CHECK(transition.visual().geometry.height == Approx((visible.height + second.height) / 2.0F));
}

TEST_CASE("context transition crossfades content when geometry is unchanged") {
  gisland::ContextTransition transition;
  const gisland::IslandGeometry geometry{340.0F, 32.0F, 16.0F};

  transition.start(geometry, geometry, 250ms, gisland::Easing::linear);
  REQUIRE(transition.active());
  transition.update(0.125F);
  CHECK(transition.visual().outgoing_opacity == Approx(0.5F));
  CHECK(transition.visual().incoming_opacity == Approx(0.5F));
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
