#include "gisland/interaction.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace gisland {

bool KeyboardActivationGate::update(bool down, bool pressed,
                                    std::chrono::milliseconds elapsed) noexcept {
  constexpr auto release_debounce = std::chrono::milliseconds{100};
  if (pressed && armed_) {
    armed_ = false;
    release_observed_ = false;
    released_for_ = {};
    return true;
  }
  if (down) {
    release_observed_ = false;
    released_for_ = {};
    return false;
  }
  if (!armed_) {
    if (!release_observed_) {
      release_observed_ = true;
      released_for_ = {};
      return false;
    }
    released_for_ += std::max(elapsed, std::chrono::milliseconds{0});
    if (released_for_ >= release_debounce) {
      armed_ = true;
    }
  }
  return false;
}

namespace {

[[nodiscard]] bool contains(const Rect &bounds, int x, int y) {
  const auto point_x = static_cast<std::int64_t>(x);
  const auto point_y = static_cast<std::int64_t>(y);
  return point_x >= bounds.x && point_y >= bounds.y &&
         point_x < static_cast<std::int64_t>(bounds.x) + bounds.width &&
         point_y < static_cast<std::int64_t>(bounds.y) + bounds.height;
}

[[nodiscard]] bool actionable(const InteractionTarget &target) {
  return target.enabled && target.clip.width > 0 && target.clip.height > 0;
}

} // namespace

const InteractionTarget *InteractionController::pointer_target(const LayoutPlan &layout, int x,
                                                               int y) const {
  for (auto target = layout.interactions.rbegin(); target != layout.interactions.rend(); ++target) {
    if (actionable(*target) && contains(target->bounds, x, y) && contains(target->clip, x, y)) {
      return &*target;
    }
  }
  return nullptr;
}

std::optional<std::string> InteractionController::pointer_action(const LayoutPlan &layout, int x,
                                                                 int y) const {
  const auto *target = pointer_target(layout, x, y);
  return target == nullptr ? std::nullopt : std::optional<std::string>{target->action_id};
}

std::optional<ButtonDecorationDrawCommand>
InteractionController::pointer_hover(const LayoutPlan &layout, int x, int y, Rgba color) const {
  const auto *target = pointer_target(layout, x, y);
  if (target == nullptr || target->kind != InteractionKind::button) {
    return std::nullopt;
  }
  return ButtonDecorationDrawCommand{target->bounds, target->clip, color, true};
}

namespace {

[[nodiscard]] bool valid_utf8(std::string_view text) {
  for (std::size_t index = 0; index < text.size();) {
    const auto first = static_cast<unsigned char>(text[index]);
    std::size_t length = 0;
    if (first <= 0x7FU) {
      length = 1;
    } else if (first >= 0xC2U && first <= 0xDFU) {
      length = 2;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      length = 3;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      length = 4;
    } else {
      return false;
    }
    if (index + length > text.size()) {
      return false;
    }
    for (std::size_t continuation = 1; continuation < length; ++continuation) {
      if ((static_cast<unsigned char>(text[index + continuation]) & 0xC0U) != 0x80U) {
        return false;
      }
    }
    if (length == 3) {
      const auto second = static_cast<unsigned char>(text[index + 1]);
      if ((first == 0xE0U && second < 0xA0U) || (first == 0xEDU && second >= 0xA0U)) {
        return false;
      }
    } else if (length == 4) {
      const auto second = static_cast<unsigned char>(text[index + 1]);
      if ((first == 0xF0U && second < 0x90U) || (first == 0xF4U && second > 0x8FU)) {
        return false;
      }
    }
    index += length;
  }
  return true;
}

[[nodiscard]] std::size_t previous_codepoint(std::string_view text, std::size_t caret) {
  if (caret == 0) {
    return 0;
  }
  std::size_t previous = caret - 1;
  while (previous > 0 && (static_cast<unsigned char>(text[previous]) & 0xC0U) == 0x80U) {
    --previous;
  }
  return previous;
}

[[nodiscard]] std::size_t next_codepoint(std::string_view text, std::size_t caret) {
  if (caret >= text.size()) {
    return text.size();
  }
  std::size_t next = caret + 1;
  while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0U) == 0x80U) {
    ++next;
  }
  return next;
}

[[nodiscard]] bool word_codepoint(std::string_view text, std::size_t offset) {
  const auto first = static_cast<unsigned char>(text[offset]);
  return first >= 0x80U || (first >= '0' && first <= '9') || (first >= 'A' && first <= 'Z') ||
         (first >= 'a' && first <= 'z') || first == '_';
}

[[nodiscard]] std::size_t previous_word(std::string_view text, std::size_t caret) {
  auto position = caret;
  while (position > 0) {
    const auto previous = previous_codepoint(text, position);
    if (word_codepoint(text, previous))
      break;
    position = previous;
  }
  while (position > 0) {
    const auto previous = previous_codepoint(text, position);
    if (!word_codepoint(text, previous))
      break;
    position = previous;
  }
  return position;
}

[[nodiscard]] std::size_t next_word(std::string_view text, std::size_t caret) {
  auto position = caret;
  while (position < text.size() && word_codepoint(text, position)) {
    position = next_codepoint(text, position);
  }
  while (position < text.size() && !word_codepoint(text, position)) {
    position = next_codepoint(text, position);
  }
  return position;
}

[[nodiscard]] std::string encode_utf8(char32_t codepoint) {
  std::string result;
  if (codepoint <= 0x7FU) {
    result.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FFU) {
    result.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
    result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else if (codepoint <= 0xFFFFU && !(codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
    result.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
    result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else if (codepoint >= 0x10000U && codepoint <= 0x10FFFFU) {
    result.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
    result.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
    result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  }
  return result;
}

[[nodiscard]] bool keyboard_result(const InteractionTarget &target) {
  return actionable(target) && target.kind != InteractionKind::text_input &&
         target.kind != InteractionKind::link;
}

} // namespace

TextInputEditor::TextInputEditor(std::string initial_value, std::size_t maximum_bytes)
    : value_(valid_utf8(initial_value) && initial_value.size() <= maximum_bytes
                 ? std::move(initial_value)
                 : std::string{}),
      caret_(value_.size()), maximum_bytes_(maximum_bytes) {}

const std::string &TextInputEditor::value() const noexcept { return value_; }

std::size_t TextInputEditor::caret() const noexcept { return caret_; }

bool TextInputEditor::replace_selection(std::string_view utf8) {
  if (!valid_utf8(utf8)) {
    return false;
  }
  const std::size_t retained = all_selected_ ? 0 : value_.size();
  if (retained + utf8.size() > maximum_bytes_) {
    return false;
  }
  if (all_selected_) {
    value_.assign(utf8);
    caret_ = value_.size();
    all_selected_ = false;
    return true;
  }
  value_.insert(caret_, utf8);
  caret_ += utf8.size();
  return !utf8.empty();
}

bool TextInputEditor::insert(char32_t codepoint) {
  const std::string encoded = encode_utf8(codepoint);
  return !encoded.empty() && replace_selection(encoded);
}

bool TextInputEditor::paste(std::string_view utf8) { return replace_selection(utf8); }

bool TextInputEditor::backspace() {
  if (all_selected_) {
    const bool changed = !value_.empty();
    value_.clear();
    caret_ = 0;
    all_selected_ = false;
    return changed;
  }
  const std::size_t previous = previous_codepoint(value_, caret_);
  if (previous == caret_) {
    return false;
  }
  value_.erase(previous, caret_ - previous);
  caret_ = previous;
  return true;
}

bool TextInputEditor::delete_forward() {
  if (all_selected_) {
    return backspace();
  }
  const std::size_t next = next_codepoint(value_, caret_);
  if (next == caret_) {
    return false;
  }
  value_.erase(caret_, next - caret_);
  return true;
}

bool TextInputEditor::delete_word_backward() {
  if (all_selected_)
    return backspace();
  const auto beginning = previous_word(value_, caret_);
  if (beginning == caret_)
    return false;
  value_.erase(beginning, caret_ - beginning);
  caret_ = beginning;
  return true;
}

bool TextInputEditor::delete_word_forward() {
  if (all_selected_)
    return backspace();
  const auto ending = next_word(value_, caret_);
  if (ending == caret_)
    return false;
  value_.erase(caret_, ending - caret_);
  return true;
}

bool TextInputEditor::delete_to_start() {
  if (all_selected_)
    return backspace();
  if (caret_ == 0)
    return false;
  value_.erase(0, caret_);
  caret_ = 0;
  return true;
}

bool TextInputEditor::delete_to_end() {
  if (all_selected_)
    return backspace();
  if (caret_ == value_.size())
    return false;
  value_.erase(caret_);
  return true;
}

void TextInputEditor::move_left() {
  all_selected_ = false;
  caret_ = previous_codepoint(value_, caret_);
}

void TextInputEditor::move_right() {
  all_selected_ = false;
  caret_ = next_codepoint(value_, caret_);
}

void TextInputEditor::move_word_left() {
  all_selected_ = false;
  caret_ = previous_word(value_, caret_);
}

void TextInputEditor::move_word_right() {
  all_selected_ = false;
  caret_ = next_word(value_, caret_);
}

void TextInputEditor::home() {
  all_selected_ = false;
  caret_ = 0;
}

void TextInputEditor::end() {
  all_selected_ = false;
  caret_ = value_.size();
}

void TextInputEditor::select_all() {
  all_selected_ = true;
  caret_ = value_.size();
}

void KeyboardNavigationController::reconcile(const LayoutPlan &layout) {
  const auto input = std::ranges::find_if(layout.interactions, [](const auto &target) {
    return actionable(target) && target.kind == InteractionKind::text_input;
  });
  input_id_ = input == layout.interactions.end() ? std::nullopt
                                                 : std::optional<std::string>{input->input_id};
  const auto selected = std::ranges::find_if(layout.interactions, [&](const auto &target) {
    return keyboard_result(target) && selected_action_id_ &&
           target.action_id == *selected_action_id_;
  });
  if (selected == layout.interactions.end()) {
    const auto first = std::ranges::find_if(layout.interactions, keyboard_result);
    selected_action_id_ = first == layout.interactions.end()
                              ? std::nullopt
                              : std::optional<std::string>{first->action_id};
  } else if (pending_result_direction_ != 0) {
    std::vector<const InteractionTarget *> results;
    for (const auto &target : layout.interactions) {
      if (keyboard_result(target)) {
        results.push_back(&target);
      }
    }
    const auto current = std::ranges::find_if(
        results, [&](const auto *target) { return target->action_id == *selected_action_id_; });
    if (current != results.end()) {
      const auto index = static_cast<std::size_t>(std::distance(results.begin(), current));
      if (pending_result_direction_ < 0 && index > 0) {
        selected_action_id_ = results[index - 1]->action_id;
      } else if (pending_result_direction_ > 0 && index + 1 < results.size()) {
        selected_action_id_ = results[index + 1]->action_id;
      }
    }
  }
  pending_result_direction_ = 0;
  if (!focused_action_id_) {
    focused_action_id_ = input == layout.interactions.end()
                             ? selected_action_id_
                             : std::optional<std::string>{input->action_id};
  }
}

bool KeyboardNavigationController::move_result(const LayoutPlan &layout, int direction) {
  std::vector<const InteractionTarget *> results;
  for (const auto &target : layout.interactions) {
    if (keyboard_result(target)) {
      results.push_back(&target);
    }
  }
  if (results.empty()) {
    selected_action_id_.reset();
    return false;
  }
  const auto current = std::ranges::find_if(results, [&](const auto *target) {
    return selected_action_id_ && target->action_id == *selected_action_id_;
  });
  std::size_t index = current == results.end()
                          ? 0
                          : static_cast<std::size_t>(std::distance(results.begin(), current));
  if (direction < 0) {
    if (index == 0) {
      return true;
    }
    --index;
  } else if (direction > 0) {
    if (index + 1 == results.size()) {
      return true;
    }
    ++index;
  }
  selected_action_id_ = results[index]->action_id;
  return false;
}

void KeyboardNavigationController::continue_result_after_republish(int direction) {
  pending_result_direction_ = direction;
}

void KeyboardNavigationController::move_focus(const LayoutPlan &layout, bool reverse) {
  std::vector<const InteractionTarget *> targets;
  for (const auto &target : layout.interactions) {
    if (actionable(target) && target.kind != InteractionKind::link) {
      targets.push_back(&target);
    }
  }
  if (targets.empty()) {
    focused_action_id_.reset();
    return;
  }
  const auto current = std::ranges::find_if(targets, [&](const auto *target) {
    return focused_action_id_ && target->action_id == *focused_action_id_;
  });
  std::size_t index = current == targets.end()
                          ? 0
                          : static_cast<std::size_t>(std::distance(targets.begin(), current));
  if (reverse) {
    index = index == 0 ? targets.size() - 1 : index - 1;
  } else {
    index = (index + 1) % targets.size();
  }
  focused_action_id_ = targets[index]->action_id;
  if (targets[index]->kind != InteractionKind::text_input) {
    selected_action_id_ = targets[index]->action_id;
  }
}

const InteractionTarget *
KeyboardNavigationController::activation_target(const LayoutPlan &layout) const {
  if (focused_action_id_) {
    const auto focused = std::ranges::find_if(layout.interactions, [&](const auto &candidate) {
      return keyboard_result(candidate) && candidate.action_id == *focused_action_id_;
    });
    if (focused != layout.interactions.end()) {
      return &*focused;
    }
  }
  if (selected_action_id_) {
    const auto target = std::ranges::find_if(layout.interactions, [&](const auto &candidate) {
      return keyboard_result(candidate) && candidate.action_id == *selected_action_id_;
    });
    if (target != layout.interactions.end()) {
      return &*target;
    }
  }
  return input_target(layout);
}

const InteractionTarget *
KeyboardNavigationController::input_target(const LayoutPlan &layout) const {
  if (!input_id_) {
    return nullptr;
  }
  const auto target = std::ranges::find_if(layout.interactions, [&](const auto &candidate) {
    return actionable(candidate) && candidate.kind == InteractionKind::text_input &&
           candidate.input_id == *input_id_;
  });
  return target == layout.interactions.end() ? nullptr : &*target;
}

std::optional<ButtonDecorationDrawCommand>
KeyboardNavigationController::selection_highlight(const LayoutPlan &layout, Rgba color) const {
  const auto *target = activation_target(layout);
  if (target == nullptr || target->kind == InteractionKind::text_input) {
    return std::nullopt;
  }
  return ButtonDecorationDrawCommand{target->bounds, target->clip, color, true};
}

const std::optional<std::string> &
KeyboardNavigationController::selected_action_id() const noexcept {
  return selected_action_id_;
}

} // namespace gisland
