#pragma once

#include "gisland/layout.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace gisland {

class InteractionController final {
public:
  [[nodiscard]] const InteractionTarget *pointer_target(const LayoutPlan &layout, int x,
                                                        int y) const;
  [[nodiscard]] std::optional<std::string> pointer_action(const LayoutPlan &layout, int x,
                                                          int y) const;
  [[nodiscard]] std::optional<ButtonDecorationDrawCommand>
  pointer_hover(const LayoutPlan &layout, int x, int y, Rgba color) const;
};

class TextInputEditor final {
public:
  explicit TextInputEditor(std::string initial_value = {}, std::size_t maximum_bytes = 1024);

  [[nodiscard]] const std::string &value() const noexcept;
  [[nodiscard]] std::size_t caret() const noexcept;
  [[nodiscard]] bool insert(char32_t codepoint);
  [[nodiscard]] bool paste(std::string_view utf8);
  [[nodiscard]] bool backspace();
  [[nodiscard]] bool delete_forward();
  [[nodiscard]] bool delete_word_backward();
  [[nodiscard]] bool delete_word_forward();
  [[nodiscard]] bool delete_to_start();
  [[nodiscard]] bool delete_to_end();
  void move_left();
  void move_right();
  void move_word_left();
  void move_word_right();
  void home();
  void end();
  void select_all();

private:
  [[nodiscard]] bool replace_selection(std::string_view utf8);

  std::string value_;
  std::size_t caret_{};
  std::size_t maximum_bytes_{};
  bool all_selected_{};
};

class KeyboardActivationGate final {
public:
  [[nodiscard]] bool update(bool pressed, std::chrono::milliseconds elapsed) noexcept;

private:
  bool armed_{true};
  bool release_observed_{};
  std::chrono::milliseconds released_for_{};
};

class KeyboardNavigationController final {
public:
  void reconcile(const LayoutPlan &layout);
  [[nodiscard]] bool move_result(const LayoutPlan &layout, int direction);
  void continue_result_after_republish(int direction);
  void move_focus(const LayoutPlan &layout, bool reverse);
  [[nodiscard]] const InteractionTarget *activation_target(const LayoutPlan &layout) const;
  [[nodiscard]] const InteractionTarget *input_target(const LayoutPlan &layout) const;
  [[nodiscard]] std::optional<ButtonDecorationDrawCommand>
  selection_highlight(const LayoutPlan &layout, Rgba color) const;
  [[nodiscard]] const std::optional<std::string> &selected_action_id() const noexcept;

private:
  std::optional<std::string> selected_action_id_;
  std::optional<std::string> focused_action_id_;
  std::optional<std::string> input_id_;
  int pending_result_direction_{};
};

} // namespace gisland
