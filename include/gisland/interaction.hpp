#pragma once

#include "gisland/layout.hpp"

#include <optional>
#include <string>

namespace gisland {

class InteractionController final {
public:
  [[nodiscard]] std::optional<std::string> pointer_action(const LayoutPlan &layout, int x,
                                                          int y) const;
};

} // namespace gisland
