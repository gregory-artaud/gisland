#pragma once

#include "gisland/display.hpp"
#include "gisland/island.hpp"

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gisland {

enum class PlatformOperation {
  connect_display,
  attach_window,
  query_outputs,
  update_input_region,
  poll_events
};

enum class PlatformErrorSeverity { recoverable, fatal };

struct PlatformError {
  PlatformOperation operation;
  PlatformErrorSeverity severity;
  std::string message;
};

enum class PlatformEventKind { output_topology_changed };

struct PlatformEvent {
  PlatformEventKind kind;
};

struct InputRegion {
  IslandGeometry geometry;
  IslandPlacement placement;
};

class PlatformHost {
public:
  virtual ~PlatformHost() = default;

  [[nodiscard]] virtual std::expected<OutputSelection, PlatformError>
  select_output(std::string_view requested_name) const = 0;
  [[nodiscard]] virtual std::expected<void, PlatformError>
  update_input_region(const InputRegion &region) const = 0;
  [[nodiscard]] virtual std::expected<std::vector<PlatformEvent>, PlatformError> poll_events() = 0;
};

using PlatformHostPtr = std::unique_ptr<PlatformHost>;

} // namespace gisland
