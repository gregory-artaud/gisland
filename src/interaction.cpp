#include "gisland/interaction.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace gisland {
namespace {

[[nodiscard]] bool contains(const Rect &bounds, int x, int y) {
  const auto point_x = static_cast<std::int64_t>(x);
  const auto point_y = static_cast<std::int64_t>(y);
  return point_x >= bounds.x && point_y >= bounds.y &&
         point_x < static_cast<std::int64_t>(bounds.x) + bounds.width &&
         point_y < static_cast<std::int64_t>(bounds.y) + bounds.height;
}

[[nodiscard]] bool focusable(const InteractionTarget &target) {
  return target.enabled && target.clip.width > 0 && target.clip.height > 0;
}

} // namespace

void InteractionController::reset(const LayoutPlan &layout) {
  const auto first = std::ranges::find_if(layout.interactions, focusable);
  if (first == layout.interactions.end()) {
    focused_index_.reset();
    return;
  }
  focused_index_ = static_cast<std::size_t>(first - layout.interactions.begin());
}

void InteractionController::clear() noexcept { focused_index_.reset(); }

void InteractionController::move_focus(const LayoutPlan &layout, FocusDirection direction) {
  if (layout.interactions.empty()) {
    focused_index_.reset();
    return;
  }
  const std::size_t count = layout.interactions.size();
  const bool valid_focus = focused_index_ && *focused_index_ < count;
  std::size_t candidate =
      valid_focus ? *focused_index_ : (direction == FocusDirection::forward ? count - 1 : 0);
  for (std::size_t checked = 0; checked < count; ++checked) {
    candidate = direction == FocusDirection::forward ? (candidate + 1) % count
                                                     : (candidate + count - 1) % count;
    if (focusable(layout.interactions[candidate])) {
      focused_index_ = candidate;
      return;
    }
  }
  focused_index_.reset();
}

std::optional<std::string> InteractionController::pointer_action(const LayoutPlan &layout, int x,
                                                                 int y) const {
  for (auto target = layout.interactions.rbegin(); target != layout.interactions.rend(); ++target) {
    if (focusable(*target) && contains(target->bounds, x, y) && contains(target->clip, x, y)) {
      return target->action_id;
    }
  }
  return std::nullopt;
}

std::optional<std::string> InteractionController::activate(const LayoutPlan &layout) const {
  if (!focused_index_ || *focused_index_ >= layout.interactions.size()) {
    return std::nullopt;
  }
  const auto &target = layout.interactions[*focused_index_];
  return focusable(target) ? std::optional{target.action_id} : std::nullopt;
}

std::optional<std::size_t> InteractionController::focused_index() const noexcept {
  return focused_index_;
}

} // namespace gisland
