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

TEST_CASE("text input editor edits UTF-8 at codepoint boundaries and enforces its byte limit") {
  gisland::TextInputEditor editor{"a\xC3\xA9", 7};
  CHECK(editor.caret() == 3);
  editor.move_left();
  REQUIRE(editor.insert(U'🙂'));
  CHECK(editor.value() == "a🙂é");
  CHECK_FALSE(editor.insert(U'x'));
  REQUIRE(editor.backspace());
  CHECK(editor.value() == "aé");
  editor.home();
  REQUIRE(editor.delete_forward());
  CHECK(editor.value() == "é");
  editor.select_all();
  REQUIRE(editor.paste("ok"));
  CHECK(editor.value() == "ok");
  CHECK_FALSE(editor.paste("\xFF"));
}

TEST_CASE("text input editor supports word and line control motions") {
  gisland::TextInputEditor editor{"alpha beta café"};
  editor.move_word_left();
  CHECK(editor.caret() == 11);
  REQUIRE(editor.delete_word_backward());
  CHECK(editor.value() == "alpha café");
  editor.home();
  editor.move_word_right();
  CHECK(editor.caret() == 6);
  REQUIRE(editor.delete_word_forward());
  CHECK(editor.value() == "alpha ");
  REQUIRE(editor.delete_to_start());
  CHECK(editor.value().empty());
  REQUIRE(editor.paste("one two"));
  editor.home();
  REQUIRE(editor.delete_to_end());
  CHECK(editor.value().empty());
}

TEST_CASE("keyboard navigation preserves result actions across live layout updates") {
  const auto first = plan({
      gisland::InteractionTarget{{0, 0, 100, 20},
                                 {0, 0, 100, 20},
                                 "query",
                                 true,
                                 "Search",
                                 gisland::InteractionKind::text_input,
                                 "query"},
      target({0, 20, 100, 20}, {0, 20, 100, 20}, "terminal"),
      target({0, 40, 100, 20}, {0, 40, 100, 20}, "browser"),
  });
  gisland::KeyboardNavigationController navigation;
  navigation.reconcile(first);
  CHECK(navigation.selected_action_id() == std::optional<std::string>{"terminal"});
  CHECK_FALSE(navigation.move_result(first, 1));
  CHECK(navigation.selected_action_id() == std::optional<std::string>{"browser"});

  const auto republished = plan({
      gisland::InteractionTarget{{0, 0, 100, 20},
                                 {0, 0, 100, 20},
                                 "query",
                                 true,
                                 "Search",
                                 gisland::InteractionKind::text_input,
                                 "query"},
      target({0, 20, 100, 20}, {0, 20, 100, 20}, "files"),
      target({0, 40, 100, 20}, {0, 40, 100, 20}, "browser"),
  });
  navigation.reconcile(republished);
  CHECK(navigation.selected_action_id() == std::optional<std::string>{"browser"});
  REQUIRE(navigation.activation_target(republished) != nullptr);
  CHECK(navigation.activation_target(republished)->action_id == "browser");

  const auto removed = plan({
      gisland::InteractionTarget{{0, 0, 100, 20},
                                 {0, 0, 100, 20},
                                 "query",
                                 true,
                                 "Search",
                                 gisland::InteractionKind::text_input,
                                 "query"},
      target({0, 20, 100, 20}, {0, 20, 100, 20}, "files"),
  });
  navigation.reconcile(removed);
  CHECK(navigation.selected_action_id() == std::optional<std::string>{"files"});
}

TEST_CASE("keyboard navigation continues into a republished result window") {
  const auto first = plan({
      target({0, 20, 100, 20}, {0, 20, 100, 20}, "terminal"),
      target({0, 40, 100, 20}, {0, 40, 100, 20}, "browser"),
  });
  gisland::KeyboardNavigationController navigation;
  navigation.reconcile(first);
  CHECK_FALSE(navigation.move_result(first, 1));
  REQUIRE(navigation.move_result(first, 1));
  navigation.continue_result_after_republish(1);

  const auto scrolled = plan({
      target({0, 20, 100, 20}, {0, 20, 100, 20}, "browser"),
      target({0, 40, 100, 20}, {0, 40, 100, 20}, "calculator"),
  });
  navigation.reconcile(scrolled);
  CHECK(navigation.selected_action_id() == std::optional<std::string>{"calculator"});
}

TEST_CASE("keyboard activation requires a release before another activation") {
  gisland::KeyboardActivationGate gate;

  CHECK_FALSE(gate.update(false));
  CHECK(gate.update(true));
  CHECK_FALSE(gate.update(true));
  CHECK_FALSE(gate.update(false));
  CHECK(gate.update(true));
}
