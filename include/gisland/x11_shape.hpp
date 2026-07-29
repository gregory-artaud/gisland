#pragma once

#include "gisland/island.hpp"

namespace gisland {

class RoundedWindowShape final {
public:
  RoundedWindowShape();
  ~RoundedWindowShape();

  RoundedWindowShape(const RoundedWindowShape &) = delete;
  RoundedWindowShape &operator=(const RoundedWindowShape &) = delete;
  RoundedWindowShape(RoundedWindowShape &&) = delete;
  RoundedWindowShape &operator=(RoundedWindowShape &&) = delete;

  void apply(void *native_window_handle, const IslandGeometry &geometry,
             const IslandPlacement &placement) const;

private:
  void *display_;
  bool available_;
};

} // namespace gisland
