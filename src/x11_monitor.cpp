#include "gisland/x11_monitor.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace gisland {

std::expected<MonitorSelection, MonitorError> select_monitor(std::span<const X11Monitor> monitors,
                                                             std::string_view requested_name) {
  const auto active = [](const X11Monitor &monitor) {
    return monitor.width > 0 && monitor.height > 0;
  };
  const auto exact = std::find_if(monitors.begin(), monitors.end(), [&](const X11Monitor &monitor) {
    return active(monitor) && monitor.name == requested_name;
  });
  if (requested_name != "primary" && exact != monitors.end()) {
    return MonitorSelection{*exact, false};
  }

  const auto primary =
      std::find_if(monitors.begin(), monitors.end(),
                   [&](const X11Monitor &monitor) { return active(monitor) && monitor.primary; });
  if (primary != monitors.end()) {
    return MonitorSelection{*primary, requested_name != "primary"};
  }

  const auto first = std::find_if(monitors.begin(), monitors.end(), active);
  if (first != monitors.end()) {
    return MonitorSelection{*first, true};
  }
  return std::unexpected(
      MonitorError{MonitorErrorCode::no_active_outputs, "X11 has no active outputs"});
}

std::expected<X11WindowPlacement, MonitorError>
place_on_monitor(const X11Monitor &monitor, int window_width, int window_height, int top_margin) {
  if (monitor.width <= 0 || monitor.height <= 0 || window_width <= 0 || window_height <= 0) {
    return std::unexpected(MonitorError{MonitorErrorCode::no_active_outputs,
                                        "window or output dimensions are invalid"});
  }
  const auto x = static_cast<std::int64_t>(monitor.x) +
                 ((static_cast<std::int64_t>(monitor.width) - window_width) / 2);
  const auto y = static_cast<std::int64_t>(monitor.y) + top_margin;
  constexpr auto minimum = static_cast<std::int64_t>(std::numeric_limits<int>::min());
  constexpr auto maximum = static_cast<std::int64_t>(std::numeric_limits<int>::max());
  if (x < minimum || x > maximum || y < minimum || y > maximum) {
    return std::unexpected(MonitorError{MonitorErrorCode::no_active_outputs,
                                        "window placement exceeds integer bounds"});
  }
  return X11WindowPlacement{static_cast<int>(x), static_cast<int>(y)};
}

} // namespace gisland
