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

gisland::InteractionTarget
target(gisland::Rect bounds, gisland::Rect clip, std::string action, bool enabled = true,
       gisland::InteractionKind kind = gisland::InteractionKind::button) {
  return gisland::InteractionTarget{bounds, clip, std::move(action), enabled, "label", kind};
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
  REQUIRE(controller.pointer_target(layout, 8, 8) != nullptr);
  CHECK(controller.pointer_target(layout, 8, 8)->action_id == "top");
}

TEST_CASE("interaction hit testing ignores disabled targets") {
  const auto disabled = plan({target({0, 0, 10, 10}, {0, 0, 10, 10}, "off", false)});
  gisland::InteractionController controller;

  CHECK(controller.pointer_action(disabled, 5, 5) == std::nullopt);
  CHECK(controller.pointer_target(disabled, 5, 5) == nullptr);
}

TEST_CASE("hover decoration is emitted only for the winning enabled button") {
  const auto layout = plan({
      target({0, 0, 20, 20}, {2, 2, 16, 16}, "button"),
      target({4, 4, 8, 8}, {4, 4, 8, 8}, "link", true, gisland::InteractionKind::link),
  });
  gisland::InteractionController controller;
  const gisland::Rgba overlay{255, 255, 255, 20};

  CHECK_FALSE(controller.pointer_hover(layout, 6, 6, overlay).has_value());
  const auto hover = controller.pointer_hover(layout, 3, 3, overlay);
  REQUIRE(hover.has_value());
  const auto command = hover.value_or(gisland::ButtonDecorationDrawCommand{});
  CHECK(command.bounds == gisland::Rect{0, 0, 20, 20});
  CHECK(command.clip == gisland::Rect{2, 2, 16, 16});
  CHECK(command.color == overlay);
  CHECK(command.enabled);
}
