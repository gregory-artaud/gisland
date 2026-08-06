#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace gisland {

struct X11Monitor {
  std::string name;
  int x;
  int y;
  int width;
  int height;
  bool primary;
};

enum class MonitorErrorCode { no_active_outputs };

struct MonitorError {
  MonitorErrorCode code;
  std::string message;
};

struct MonitorSelection {
  X11Monitor monitor;
  bool used_fallback;
};

struct X11WindowPlacement {
  int x;
  int y;

  bool operator==(const X11WindowPlacement &) const = default;
};

struct X11CanvasGeometry {
  int width;
  int height;
  int surface_x;
  int surface_y;
  int surface_width;
};

[[nodiscard]] std::expected<MonitorSelection, MonitorError>
select_monitor(std::span<const X11Monitor> monitors, std::string_view requested_name);

[[nodiscard]] std::expected<X11WindowPlacement, MonitorError>
place_on_monitor(const X11Monitor &monitor, int window_width, int window_height, int top_margin);

[[nodiscard]] std::expected<X11WindowPlacement, MonitorError>
place_on_monitor(const X11Monitor &monitor, const X11CanvasGeometry &canvas, int top_margin);

} // namespace gisland
