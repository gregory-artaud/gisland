#pragma once

#include "gisland/island.hpp"
#include "gisland/x11_monitor.hpp"

#include <cstdint>
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
  pointer_grab_failed,
  request_failed
};

struct X11WindowError {
  X11WindowErrorCode code;
  std::string message;
};

struct X11RootBounds {
  int x;
  int y;
  int width;
  int height;
};

enum class X11WindowEventKind { inside_press, outside_press, focus_lost, topology_changed };

struct X11WindowEvent {
  X11WindowEventKind kind;
  PointerButton button{PointerButton::other};
  std::uint64_t timestamp{};
  int x{};
  int y{};
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
  apply_shape(const IslandGeometry &geometry) const;
  [[nodiscard]] std::expected<void, X11WindowError> enter_expanded(std::uint64_t timestamp = 0);
  [[nodiscard]] std::expected<void, X11WindowError> leave_expanded(bool restore_focus);
  [[nodiscard]] std::expected<std::vector<X11WindowEvent>, X11WindowError> poll_events();

private:
  struct Impl;
  explicit X11WindowHost(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

} // namespace gisland
