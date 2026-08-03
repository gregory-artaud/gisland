#include "gisland/interaction.hpp"

#include "gisland/layout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

namespace {

gisland::LayoutPlan plan(std::vector<gisland::InteractionTarget> targets) {
  return gisland::LayoutPlan{
      .view = {},
      .content = {},
      .interactions = std::move(targets),
  };
}

gisland::InteractionTarget target(gisland::Rect bounds, gisland::Rect clip, std::string action,
                                  bool enabled = true) {
  return gisland::InteractionTarget{bounds, clip, std::move(action), enabled, "label"};
}

} // namespace

TEST_CASE("interaction hit testing uses clipped half-open bounds and topmost order") {
  const auto layout = plan({
      target({0, 0, 20, 20}, {5, 5, 10, 10}, "under"),
      target({8, 8, 10, 10}, {8, 8, 10, 10}, "top"),
  });
  gisland::InteractionController controller;

  CHECK(controller.pointer_action(layout, 4, 5) == std::nullopt);
  CHECK(controller.pointer_action(layout, 5, 5) == std::optional<std::string>{"under"});
  CHECK(controller.pointer_action(layout, 8, 8) == std::optional<std::string>{"top"});
  CHECK(controller.pointer_action(layout, 18, 8) == std::nullopt);
}

TEST_CASE("interaction focus skips disabled targets and wraps in both directions") {
  const auto layout = plan({
      target({0, 0, 10, 10}, {0, 0, 10, 10}, "first"),
      target({10, 0, 10, 10}, {10, 0, 10, 10}, "disabled", false),
      target({20, 0, 10, 10}, {20, 0, 10, 10}, "last"),
  });
  gisland::InteractionController controller;

  controller.reset(layout);
  CHECK(controller.focused_index() == std::optional<std::size_t>{0});
  CHECK(controller.activate(layout) == std::optional<std::string>{"first"});
  controller.move_focus(layout, gisland::FocusDirection::forward);
  CHECK(controller.focused_index() == std::optional<std::size_t>{2});
  controller.move_focus(layout, gisland::FocusDirection::forward);
  CHECK(controller.focused_index() == std::optional<std::size_t>{0});
  controller.move_focus(layout, gisland::FocusDirection::backward);
  CHECK(controller.focused_index() == std::optional<std::size_t>{2});
}

TEST_CASE("interaction reset and clear safely handle layouts without enabled targets") {
  const auto disabled = plan({target({0, 0, 10, 10}, {0, 0, 10, 10}, "off", false)});
  const auto enabled = plan({target({0, 0, 10, 10}, {0, 0, 10, 10}, "on")});
  gisland::InteractionController controller;

  controller.reset(disabled);
  CHECK(controller.focused_index() == std::nullopt);
  CHECK(controller.activate(disabled) == std::nullopt);
  CHECK(controller.pointer_action(disabled, 5, 5) == std::nullopt);

  controller.reset(enabled);
  REQUIRE(controller.focused_index().has_value());
  controller.clear();
  CHECK(controller.focused_index() == std::nullopt);
}
