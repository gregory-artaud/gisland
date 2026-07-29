#pragma once

#include "gisland/config.hpp"
#include "gisland/context.hpp"

#include <deque>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace gisland {

enum class ModuleState { stopped, starting, running, backoff, stopping, failed };

enum class StopCause {
  requested,
  clean_exit,
  failed_exit,
  signal,
  spawn_error,
  handshake_timeout,
  protocol_violation,
  io_error,
  unresponsive
};

enum class ShutdownSignal { terminate, kill };

struct StateTransition {
  ModuleState from;
  ModuleState to;
  StopCause cause;
  MonotonicTime at;
};

struct LifecycleError {
  ModuleState state;
  std::string operation;
};

class ModuleLifecycle {
public:
  ModuleLifecycle(RestartPolicy policy, ModuleTimings timings);

  [[nodiscard]] ModuleState state() const noexcept;
  [[nodiscard]] std::optional<MonotonicTime> restart_at() const noexcept;

  [[nodiscard]] std::expected<StateTransition, LifecycleError> start(MonotonicTime now);
  [[nodiscard]] std::expected<StateTransition, LifecycleError> ready(MonotonicTime now);
  [[nodiscard]] std::vector<StateTransition> tick(MonotonicTime now);
  [[nodiscard]] std::vector<StateTransition> exited(StopCause cause, MonotonicTime now);
  [[nodiscard]] std::expected<StateTransition, LifecycleError> stop(MonotonicTime now);
  [[nodiscard]] std::expected<StateTransition, LifecycleError> fail(StopCause cause,
                                                                    MonotonicTime now);

  [[nodiscard]] std::optional<ShutdownSignal> due_signal(MonotonicTime now) const noexcept;
  [[nodiscard]] std::expected<void, LifecycleError> signal_sent(ShutdownSignal signal,
                                                                MonotonicTime now);

private:
  [[nodiscard]] StateTransition transition_to(ModuleState next, StopCause cause,
                                              MonotonicTime now);
  void begin_stopping(StopCause cause, MonotonicTime signal_at);
  void clear_process_state();
  void reset_backoff_if_healthy(MonotonicTime now);
  [[nodiscard]] bool record_failure(MonotonicTime now);
  [[nodiscard]] bool should_restart(StopCause cause) const noexcept;
  [[nodiscard]] static bool is_failure(StopCause cause) noexcept;

  RestartPolicy policy_;
  ModuleTimings timings_;
  ModuleState state_{ModuleState::stopped};
  std::chrono::milliseconds next_backoff_;
  std::optional<MonotonicTime> handshake_deadline_;
  std::optional<MonotonicTime> restart_at_;
  std::optional<MonotonicTime> running_since_;
  std::optional<StopCause> pending_stop_cause_;
  std::optional<ShutdownSignal> pending_signal_;
  std::optional<MonotonicTime> signal_at_;
  std::deque<MonotonicTime> failures_;
};

} // namespace gisland
