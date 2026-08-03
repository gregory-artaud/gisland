#include "gisland/interaction.hpp"

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

[[nodiscard]] bool actionable(const InteractionTarget &target) {
  return target.enabled && target.clip.width > 0 && target.clip.height > 0;
}

} // namespace

std::optional<std::string> InteractionController::pointer_action(const LayoutPlan &layout, int x,
                                                                 int y) const {
  for (auto target = layout.interactions.rbegin(); target != layout.interactions.rend(); ++target) {
    if (actionable(*target) && contains(target->bounds, x, y) && contains(target->clip, x, y)) {
      return target->action_id;
    }
  }
  return std::nullopt;
}

} // namespace gisland
