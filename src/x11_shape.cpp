#include "gisland/x11_shape.hpp"

#include <X11/Xlib.h>
#include <X11/extensions/shape.h>

#include <cmath>
#include <vector>

namespace gisland {

RoundedWindowShape::RoundedWindowShape() : display_(XOpenDisplay(nullptr)), available_(false) {
  if (display_ != nullptr) {
    int event_base = 0;
    int error_base = 0;
    available_ =
        XShapeQueryExtension(static_cast<Display *>(display_), &event_base, &error_base) != 0;
  }
}

RoundedWindowShape::~RoundedWindowShape() {
  if (display_ != nullptr) {
    XCloseDisplay(static_cast<Display *>(display_));
  }
}

void RoundedWindowShape::apply(void *native_window_handle, const IslandGeometry &geometry,
                               const IslandPlacement &placement) const {
  if (!available_ || native_window_handle == nullptr) {
    return;
  }

  const Window window = *static_cast<const XID *>(native_window_handle);
  const auto mask_rows = rounded_mask_rows(geometry);
  std::vector<XRectangle> rectangles;
  rectangles.reserve(mask_rows.size());
  for (const auto &row : mask_rows) {
    rectangles.push_back({
        .x = static_cast<short>(row.x),
        .y = static_cast<short>(row.y),
        .width = static_cast<unsigned short>(row.width),
        .height = static_cast<unsigned short>(row.height),
    });
  }

  auto *display = static_cast<Display *>(display_);
  XShapeCombineRectangles(display, window, ShapeInput, static_cast<int>(std::lround(placement.x)),
                          static_cast<int>(std::lround(placement.y)), rectangles.data(),
                          static_cast<int>(rectangles.size()), ShapeSet, Unsorted);
  XFlush(display);
}

} // namespace gisland
