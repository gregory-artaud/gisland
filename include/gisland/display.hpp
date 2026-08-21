#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace gisland {

struct DisplayOutput {
  std::string name;
  int x;
  int y;
  int width;
  int height;
  bool preferred;
};

enum class DisplayErrorCode { no_active_outputs };

struct DisplayError {
  DisplayErrorCode code;
  std::string message;
};

struct OutputSelection {
  DisplayOutput output;
  bool used_fallback;
};

struct AbsolutePlacement {
  int x;
  int y;

  bool operator==(const AbsolutePlacement &) const = default;
};

struct CanvasGeometry {
  int width;
  int height;
  int surface_x;
  int surface_y;
  int surface_width;
};

[[nodiscard]] std::expected<OutputSelection, DisplayError>
select_output(std::span<const DisplayOutput> outputs, std::string_view requested_name);

[[nodiscard]] std::expected<AbsolutePlacement, DisplayError>
place_canvas(const DisplayOutput &output, int window_width, int window_height, int top_margin);

[[nodiscard]] std::expected<AbsolutePlacement, DisplayError>
place_canvas(const DisplayOutput &output, const CanvasGeometry &canvas, int top_margin);

} // namespace gisland
