#include "gisland/context.hpp"
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

TEST_CASE("fixed canvas contains every named compact style") {
  constexpr std::string_view theme_text = R"(
[palette]
surface = "#000000"
foreground = "#FFFFFF"
muted = "#808080"
accent = "#7C5CFC"
success = "#30D158"
warning = "#FFD60A"
error = "#FF453A"
[fonts]
ui = "/tmp/ui.ttf"
[typography.body]
font = "ui"
color = "foreground"
size = 16
weight = 400
line_height = 1
[images.test]
width = 16
height = 16
fit = "contain"
shape = "rectangle"
[gaps]
normal = 8
[spacers]
normal = 8
[view.compact]
padding = 4
radius = 16
border = 0
min_width = 32
max_width = 100
min_height = 32
max_height = 32
[view.compact.styles.wide-hud]
padding = 4
radius = 20
border = 0
min_width = 1000
max_width = 1000
min_height = 68
max_height = 68
[view.expanded]
padding = 8
radius = 24
border = 0
min_width = 200
max_width = 400
min_height = 100
max_height = 300
[progress]
ring_diameter = 32
ring_thickness = 4
compact_height = 48
track = "muted"
[shadow]
offset_x = 0
offset_y = 0
blur = 0
spread = 0
color = "#00000000"
[animation]
compact_to_expanded_ms = 0
context_change_ms = 0
easing = "linear"
[animation.progress]
duration_ms = 270
easing = "ease-out"
[animation.reduced_motion]
compact_to_expanded_ms = 0
context_change_ms = 0
[animation.reduced_motion.progress]
duration_ms = 0
[icons.calendar]
font = "ui"
codepoint = 0x41
)";
  const auto theme = gisland::parse_theme(theme_text, "canvas-theme.toml");
  INFO((theme.has_value() ? std::string{} : theme.error().path + ": " + theme.error().message));
  REQUIRE(theme.has_value());

  const auto canvas = gisland::fixed_canvas_for(*theme);

  CHECK(canvas.surface_width == Approx(1000.0F));
  CHECK(canvas.surface_height == Approx(300.0F));
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

TEST_CASE("progress animation eases and retargets from its rendered value") {
  const auto plan = [](double value, std::optional<double> transition_from) {
    return gisland::LayoutPlan{
        {},
        {gisland::ProgressDrawCommand{{0, 0, 100, 5},
                                      {0, 0, 100, 5},
                                      {0, 0, 100, 5},
                                      {0, 0, static_cast<int>(value * 100.0), 5},
                                      {},
                                      {},
                                      "/children/1",
                                      value,
                                      transition_from}},
        {}};
  };

  gisland::ProgressAnimator animator;
  const auto target = plan(0.8, 0.2);
  animator.retarget(target, 270ms, gisland::Easing::ease_out, false);
  CHECK(animator.active());
  CHECK(std::get<gisland::ProgressDrawCommand>(animator.apply(target).content[0]).fill.width == 20);

  animator.update(0.1F);
  const auto midway = animator.apply(target);
  const auto midway_width = std::get<gisland::ProgressDrawCommand>(midway.content[0]).fill.width;
  CHECK(midway_width > 20);
  CHECK(midway_width < 80);

  const auto replacement = plan(0.4, 0.8);
  animator.retarget(replacement, 270ms, gisland::Easing::ease_out, true);
  CHECK(std::get<gisland::ProgressDrawCommand>(animator.apply(replacement).content[0]).fill.width ==
        midway_width);
  animator.update(0.27F);
  CHECK_FALSE(animator.active());
  CHECK(std::get<gisland::ProgressDrawCommand>(animator.apply(replacement).content[0]).fill.width ==
        40);
}

TEST_CASE("progress animation snaps without a source or with reduced motion") {
  const gisland::LayoutPlan plan{{},
                                 {gisland::ProgressDrawCommand{{0, 0, 100, 5},
                                                               {0, 0, 100, 5},
                                                               {0, 0, 100, 5},
                                                               {0, 0, 70, 5},
                                                               {},
                                                               {},
                                                               "/progress",
                                                               0.7,
                                                               std::nullopt}},
                                 {}};
  gisland::ProgressAnimator animator;
  animator.retarget(plan, 270ms, gisland::Easing::ease_out, false);
  CHECK_FALSE(animator.active());
  CHECK(std::get<gisland::ProgressDrawCommand>(animator.apply(plan).content[0]).fill.width == 70);

  auto sourced = plan;
  std::get<gisland::ProgressDrawCommand>(sourced.content[0]).transition_from = 0.1;
  animator.retarget(sourced, 0ms, gisland::Easing::ease_out, false);
  CHECK_FALSE(animator.active());
  CHECK(std::get<gisland::ProgressDrawCommand>(animator.apply(sourced).content[0]).fill.width ==
        70);
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
  crossfade.set_mode(hover.mode(), 325ms);
  spring.update(0.05F);
  crossfade.update(0.15F);
  const float spring_before = spring.value();
  const auto expanded_before = crossfade.expanded();

  hover.update(false, 0.10F);
  CHECK(hover.mode() == gisland::IslandMode::expanded);
  hover.update(true, 0.0F);
  CHECK(hover.mode() == gisland::IslandMode::expanded);

  hover.update(false, 0.119F);
  CHECK(hover.mode() == gisland::IslandMode::expanded);
  hover.update(false, 0.001F);
  REQUIRE(hover.mode() == gisland::IslandMode::compact);
  spring.set_target(0.0F);
  crossfade.set_mode(hover.mode(), 325ms);

  CHECK(spring.value() == Approx(spring_before));
  CHECK(crossfade.compact().opacity == Approx(0.0F));
  CHECK(crossfade.expanded().opacity == Approx(expanded_before.opacity));
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

TEST_CASE("persistent reveal lasts until neutralized or its contribution disappears") {
  gisland::OverlayModeController controller{120ms};
  controller.set_reveal(true);
  controller.update(false, true, 60.0F);
  CHECK(controller.mode() == gisland::IslandMode::expanded);

  controller.close();
  CHECK(controller.mode() == gisland::IslandMode::compact);

  controller.set_reveal(true);
  controller.update(false, false, 0.0F);
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
  CHECK(expanded.blur == Approx(0.0F));
  CHECK(expanded.scale == Approx(1.0F));
}

TEST_CASE("black fade opacity has exact phase boundaries and no overlap") {
  const gisland::IslandGeometry geometry{340.0F, 32.0F, 16.0F};
  gisland::ContextTransition transition;
  transition.start(geometry, geometry, 100ms, gisland::Easing::linear);
  auto visual = transition.visual();
  CHECK(visual.outgoing_opacity == Approx(1.0F));
  CHECK(visual.incoming_opacity == Approx(0.0F));

  transition.update(0.038F);
  visual = transition.visual();
  CHECK(visual.outgoing_opacity == Approx(0.0F));
  CHECK(visual.incoming_opacity == Approx(0.0F));

  transition.update(0.01F);
  visual = transition.visual();
  CHECK(visual.outgoing_opacity == Approx(0.0F));
  CHECK(visual.incoming_opacity == Approx(0.0F));

  transition.update(0.052F);
  visual = transition.visual();
  CHECK(visual.outgoing_opacity == Approx(0.0F));
  CHECK(visual.incoming_opacity == Approx(1.0F));

  gisland::ContextTransition curved;
  curved.start(geometry, geometry, 100ms, gisland::Easing::linear);
  curved.update(0.019F);
  CHECK(curved.visual().outgoing_opacity > 0.5F);
  curved.update(0.055F);
  CHECK(curved.visual().incoming_opacity > 0.5F);

  for (int sample = -20; sample <= 120; ++sample) {
    gisland::ContextTransition sampled;
    sampled.start(geometry, geometry, 100ms, gisland::Easing::linear);
    sampled.update(static_cast<float>(sample) / 1000.0F);
    const auto opacity = sampled.visual();
    CHECK(opacity.outgoing_opacity >= 0.0F);
    CHECK(opacity.outgoing_opacity <= 1.0F);
    CHECK(opacity.incoming_opacity >= 0.0F);
    CHECK(opacity.incoming_opacity <= 1.0F);
    CHECK(opacity.outgoing_opacity * opacity.incoming_opacity == Approx(0.0F));
  }
}

TEST_CASE("content mode changes use opacity-only black fade in both directions") {
  gisland::ContentCrossfade crossfade;
  crossfade.set_mode(gisland::IslandMode::expanded, 100ms);
  crossfade.update(0.038F);

  const auto outgoing = crossfade.compact();
  CHECK(outgoing.opacity == Approx(0.0F));
  CHECK(outgoing.blur == Approx(0.0F));
  CHECK(outgoing.scale == Approx(1.0F));

  const auto black = crossfade.expanded();
  CHECK(black.opacity == Approx(0.0F));
  CHECK(black.blur == Approx(0.0F));
  CHECK(black.scale == Approx(1.0F));

  crossfade.update(0.062F);
  const auto incoming = crossfade.expanded();
  CHECK(incoming.opacity == Approx(1.0F));

  crossfade.set_mode(gisland::IslandMode::compact, 100ms);
  crossfade.update(0.038F);
  CHECK(crossfade.expanded().opacity == Approx(0.0F));
  CHECK(crossfade.compact().opacity == Approx(0.0F));
  crossfade.update(0.062F);
  CHECK(crossfade.compact().opacity == Approx(1.0F));
}

TEST_CASE("content mode changes clamp frame deltas and snap at zero duration") {
  gisland::ContentCrossfade crossfade;
  crossfade.set_mode(gisland::IslandMode::expanded, 325ms);
  crossfade.update(-1.0F);
  CHECK(crossfade.compact().opacity == Approx(1.0F));
  CHECK(crossfade.expanded().opacity == Approx(0.0F));
  crossfade.update(10.0F);
  CHECK(crossfade.compact().opacity == Approx(0.0F));
  CHECK(crossfade.expanded().opacity == Approx(1.0F));

  crossfade.set_mode(gisland::IslandMode::compact, 0ms);
  CHECK(crossfade.compact().opacity == Approx(1.0F));
  CHECK(crossfade.expanded().opacity == Approx(0.0F));
}

TEST_CASE("content mode reversal fades from displayed destination without flashing old content") {
  gisland::ContentCrossfade crossfade;
  crossfade.set_mode(gisland::IslandMode::expanded, 100ms);
  crossfade.update(0.075F);

  const auto expanded_before_reversal = crossfade.expanded();
  REQUIRE(expanded_before_reversal.opacity > 0.0F);
  crossfade.set_mode(gisland::IslandMode::compact, 100ms);

  CHECK(crossfade.compact().opacity == Approx(0.0F));
  CHECK(crossfade.expanded().opacity == Approx(expanded_before_reversal.opacity));
  crossfade.update(0.01F);
  CHECK(crossfade.expanded().opacity < expanded_before_reversal.opacity);

  crossfade.update(0.028F);
  crossfade.set_mode(gisland::IslandMode::expanded, 100ms);
  CHECK(crossfade.compact().opacity == Approx(0.0F));
  CHECK(crossfade.expanded().opacity == Approx(0.0F));
}

TEST_CASE("context transition eases geometry independently and fades content through black") {
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
  CHECK(visual.outgoing_opacity == Approx(0.0F));
  CHECK(visual.incoming_opacity > 0.0F);
  CHECK(visual.incoming_opacity < 1.0F);
  CHECK(visual.surface_progress == Approx(0.5F));

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

TEST_CASE("interrupted context fades preserve the captured visible opacity") {
  const gisland::IslandGeometry geometry{340.0F, 32.0F, 16.0F};
  for (const float interruption_progress : {0.2F, 0.43F, 0.7F}) {
    gisland::ContextTransition transition;
    transition.start(geometry, geometry, 100ms, gisland::Easing::linear);
    transition.update(interruption_progress * 0.1F);
    const auto interrupted = transition.visual();
    const float captured_opacity = interrupted.outgoing_opacity + interrupted.incoming_opacity;

    transition.start(interrupted.geometry, geometry, 100ms, gisland::Easing::linear);
    const auto restarted = transition.visual();
    CHECK(captured_opacity * restarted.outgoing_opacity == Approx(captured_opacity));
    CHECK(restarted.incoming_opacity == Approx(0.0F));
  }
}

TEST_CASE("context transition holds content black when geometry is unchanged") {
  gisland::ContextTransition transition;
  const gisland::IslandGeometry geometry{340.0F, 32.0F, 16.0F};

  transition.start(geometry, geometry, 250ms, gisland::Easing::linear);
  REQUIRE(transition.active());
  transition.update(0.1F);
  CHECK(transition.visual().outgoing_opacity == Approx(0.0F));
  CHECK(transition.visual().incoming_opacity == Approx(0.0F));
}

TEST_CASE("context transition policy aligns revisions with stable identity") {
  const std::optional<gisland::ContextKey> clock{gisland::ContextKey{"clock", "configured"}};
  const std::optional<gisland::ContextKey> history{gisland::ContextKey{"notifications", "history"}};

  CHECK(gisland::classify_context_transition(clock, history, clock, history) ==
        gisland::ContextTransitionKind::aligned_content_crossfade);
  CHECK(gisland::classify_context_transition(clock, history, clock, std::nullopt) ==
        gisland::ContextTransitionKind::full_crossfade);
  CHECK(gisland::classify_context_transition(clock, std::nullopt, clock, std::nullopt) ==
        gisland::ContextTransitionKind::aligned_content_crossfade);
}

TEST_CASE("aligned content transition keeps incoming content fully opaque") {
  gisland::ContextTransition transition;
  const gisland::IslandGeometry source{360.0F, 272.0F, 24.0F};
  const gisland::IslandGeometry target{360.0F, 232.0F, 24.0F};
  transition.start(source, target, 250ms, gisland::Easing::linear);
  transition.update(0.125F);

  const auto visual = transition.visual();
  CHECK(gisland::context_outgoing_opacity(gisland::ContextTransitionKind::aligned_content_crossfade,
                                          visual) == Approx(0.5F));
  CHECK(gisland::context_incoming_opacity(gisland::ContextTransitionKind::aligned_content_crossfade,
                                          visual) == Approx(1.0F));
  CHECK(gisland::context_outgoing_opacity(gisland::ContextTransitionKind::full_crossfade, visual) ==
        Approx(0.0F));
  CHECK(gisland::context_incoming_opacity(gisland::ContextTransitionKind::full_crossfade, visual) >
        0.0F);
}

TEST_CASE("expanded owner switch preserves outgoing compact content") {
  const std::optional<gisland::ContextKey> battery{gisland::ContextKey{"battery", "configured"}};
  const std::optional<gisland::ContextKey> clock{gisland::ContextKey{"clock", "configured"}};
  const std::optional<gisland::ContextKey> history{gisland::ContextKey{"notifications", "history"}};

  CHECK(gisland::preserve_compact_during_expanded_switch(gisland::IslandMode::compact,
                                                         gisland::IslandMode::expanded, battery,
                                                         battery, clock, history));
  CHECK_FALSE(gisland::preserve_compact_during_expanded_switch(gisland::IslandMode::compact,
                                                               gisland::IslandMode::compact,
                                                               battery, battery, clock, history));
  CHECK_FALSE(gisland::preserve_compact_during_expanded_switch(gisland::IslandMode::expanded,
                                                               gisland::IslandMode::expanded,
                                                               battery, battery, clock, history));
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
