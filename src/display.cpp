#include "gisland/display.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace gisland {

std::expected<OutputSelection, DisplayError> select_output(std::span<const DisplayOutput> outputs,
                                                           std::string_view requested_name) {
  const auto active = [](const DisplayOutput &output) {
    return output.width > 0 && output.height > 0;
  };
  const auto exact = std::find_if(outputs.begin(), outputs.end(), [&](const DisplayOutput &output) {
    return active(output) && output.name == requested_name;
  });
  if (requested_name != "primary" && exact != outputs.end()) {
    return OutputSelection{*exact, false};
  }

  const auto preferred =
      std::find_if(outputs.begin(), outputs.end(),
                   [&](const DisplayOutput &output) { return active(output) && output.preferred; });
  if (preferred != outputs.end()) {
    return OutputSelection{*preferred, requested_name != "primary"};
  }

  const auto first = std::find_if(outputs.begin(), outputs.end(), active);
  if (first != outputs.end()) {
    return OutputSelection{*first, true};
  }
  return std::unexpected(
      DisplayError{DisplayErrorCode::no_active_outputs, "display has no active outputs"});
}

std::expected<AbsolutePlacement, DisplayError>
place_canvas(const DisplayOutput &output, int window_width, int window_height, int top_margin) {
  return place_canvas(output, CanvasGeometry{window_width, window_height, 0, 0, window_width},
                      top_margin);
}

std::expected<AbsolutePlacement, DisplayError>
place_canvas(const DisplayOutput &output, const CanvasGeometry &canvas, int top_margin) {
  if (output.width <= 0 || output.height <= 0 || canvas.width <= 0 || canvas.height <= 0 ||
      canvas.surface_x < 0 || canvas.surface_y < 0 || canvas.surface_width <= 0 ||
      canvas.surface_x > canvas.width || canvas.surface_width > canvas.width - canvas.surface_x ||
      canvas.surface_y > canvas.height) {
    return std::unexpected(DisplayError{DisplayErrorCode::no_active_outputs,
                                        "window or output dimensions are invalid"});
  }
  const auto x = static_cast<std::int64_t>(output.x) +
                 ((static_cast<std::int64_t>(output.width) - canvas.surface_width) / 2) -
                 canvas.surface_x;
  const auto y = static_cast<std::int64_t>(output.y) + top_margin - canvas.surface_y;
  constexpr auto minimum = static_cast<std::int64_t>(std::numeric_limits<int>::min());
  constexpr auto maximum = static_cast<std::int64_t>(std::numeric_limits<int>::max());
  if (x < minimum || x > maximum || y < minimum || y > maximum) {
    return std::unexpected(DisplayError{DisplayErrorCode::no_active_outputs,
                                        "window placement exceeds integer bounds"});
  }
  return AbsolutePlacement{static_cast<int>(x), static_cast<int>(y)};
}

} // namespace gisland
