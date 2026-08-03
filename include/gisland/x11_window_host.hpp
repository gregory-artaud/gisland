#pragma once

#include "gisland/island.hpp"
#include "gisland/x11_monitor.hpp"

#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace gisland {

enum class X11WindowErrorCode {
  display_unavailable,
  invalid_window,
  randr_unavailable,
  no_active_outputs,
  request_failed
};

struct X11WindowError {
  X11WindowErrorCode code;
  std::string message;
};

enum class X11WindowEventKind { topology_changed };

struct X11WindowEvent {
  X11WindowEventKind kind;
};

class X11WindowHost final {
public:
  [[nodiscard]] static std::expected<X11WindowHost, X11WindowError>
  create(void *native_window_handle);

  X11WindowHost(const X11WindowHost &) = delete;
  X11WindowHost &operator=(const X11WindowHost &) = delete;
  X11WindowHost(X11WindowHost &&) noexcept;
  X11WindowHost &operator=(X11WindowHost &&) noexcept;
  ~X11WindowHost();

  [[nodiscard]] std::expected<MonitorSelection, X11WindowError>
  select_output(std::string_view requested_name) const;
  [[nodiscard]] std::expected<void, X11WindowError>
  apply_shape(const IslandGeometry &geometry, const IslandPlacement &placement) const;
  [[nodiscard]] std::expected<std::vector<X11WindowEvent>, X11WindowError> poll_events();

private:
  struct Impl;
  explicit X11WindowHost(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

} // namespace gisland
