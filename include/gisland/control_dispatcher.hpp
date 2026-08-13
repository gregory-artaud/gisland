#pragma once

#include "gisland/control.hpp"
#include "gisland/runtime.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace gisland {

using RestartRequest = std::function<std::expected<void, SupervisorCommandError>(
    std::string instance_id, std::uint64_t generation)>;
using ReloadRequest = std::function<std::expected<void, std::string>(MonotonicTime now)>;
using ActionRequest = std::function<std::expected<void, SupervisorCommandError>(
    std::string instance_id, std::uint64_t generation, ActionMessage message)>;

struct CompletedControlAction {
  PendingControlToken token;
  ControlResponse response;
};

enum class ActionEventResult { consumed, stale, protocol_error };

class ControlDispatcher final {
public:
  ControlDispatcher(RuntimeCoordinator &runtime, OverlayModeController &mode,
                    RestartRequest request_restart, std::string socket_path,
                    ReloadRequest request_reload = {}, ActionRequest request_action = {},
                    std::uint64_t first_invocation_id = 1);

  [[nodiscard]] ControlResponse dispatch(const ControlCommand &command, MonotonicTime now);
  [[nodiscard]] ControlDispatchResult dispatch_deferred(const ControlCommand &command,
                                                        MonotonicTime now);
  void consume(const RestartCompletedEvent &event);
  void consume(const ActionDeliveryEvent &event);
  [[nodiscard]] ActionEventResult consume(const ModuleMessageEvent &event);
  void consume(const ProcessExitedEvent &event);
  void consume(const ContextsRemovedEvent &event);
  void consume(const ProcessStartedEvent &event);
  void expire(MonotonicTime now);
  [[nodiscard]] bool cancel(PendingControlToken token);
  void cancel_generation(std::string_view instance_id, std::uint64_t generation);
  void cancel_instance(std::string_view instance_id);
  void cancel_all();
  [[nodiscard]] std::vector<CompletedControlAction> take_completed();
  [[nodiscard]] std::size_t pending_action_count() const noexcept;

private:
  [[nodiscard]] ControlResponse status(MonotonicTime now);
  [[nodiscard]] std::vector<ModuleControlStatus> modules(MonotonicTime now);
  [[nodiscard]] ControlResponse restart(std::string_view instance_id, MonotonicTime now);
  [[nodiscard]] ControlDispatchResult action(const ActionControl &command, MonotonicTime now);
  [[nodiscard]] std::optional<std::uint64_t> allocate_invocation_id();
  void complete(std::uint64_t invocation_id, ControlResponse response);

  struct PendingAction {
    PendingControlToken token;
    std::string instance_id;
    std::uint64_t generation;
    std::string action_id;
    std::optional<MonotonicTime> deadline;
  };

  RuntimeCoordinator &runtime_;
  OverlayModeController &mode_;
  RestartRequest request_restart_;
  ReloadRequest request_reload_;
  ActionRequest request_action_;
  std::string socket_path_;
  std::uint64_t next_restart_generation_{1};
  std::map<std::string, std::uint64_t> pending_restarts_;
  std::uint64_t next_invocation_id_;
  std::map<std::uint64_t, PendingAction> pending_actions_;
  std::vector<CompletedControlAction> completed_actions_;
};

} // namespace gisland
