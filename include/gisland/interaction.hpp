#pragma once

#include "gisland/layout.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace gisland {

enum class FocusDirection { forward, backward };

class InteractionController final {
public:
  void reset(const LayoutPlan &layout);
  void clear() noexcept;
  void move_focus(const LayoutPlan &layout, FocusDirection direction);

  [[nodiscard]] std::optional<std::string> pointer_action(const LayoutPlan &layout, int x,
                                                          int y) const;
  [[nodiscard]] std::optional<std::string> activate(const LayoutPlan &layout) const;
  [[nodiscard]] std::optional<std::size_t> focused_index() const noexcept;

private:
  std::optional<std::size_t> focused_index_;
};

} // namespace gisland
