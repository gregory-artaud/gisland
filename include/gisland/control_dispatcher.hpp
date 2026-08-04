#pragma once

#include "gisland/control.hpp"
#include "gisland/runtime.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace gisland {

using RestartRequest = std::function<std::expected<void, SupervisorCommandError>(
    std::string instance_id, std::uint64_t generation)>;

class ControlDispatcher final {
public:
  ControlDispatcher(RuntimeCoordinator &runtime, OverlayModeController &mode,
                    RestartRequest request_restart, std::string socket_path);

  [[nodiscard]] ControlResponse dispatch(const ControlCommand &command, MonotonicTime now);
  void consume(const RestartCompletedEvent &event);

private:
  [[nodiscard]] ControlResponse status(MonotonicTime now);
  [[nodiscard]] std::vector<ModuleControlStatus> modules(MonotonicTime now);
  [[nodiscard]] ControlResponse restart(std::string_view instance_id, MonotonicTime now);

  RuntimeCoordinator &runtime_;
  OverlayModeController &mode_;
  RestartRequest request_restart_;
  std::string socket_path_;
  std::uint64_t next_restart_generation_{1};
  std::map<std::string, std::uint64_t> pending_restarts_;
};

} // namespace gisland
