#pragma once

#include "gisland/layout.hpp"

#include <optional>
#include <string>

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

} // namespace gisland
