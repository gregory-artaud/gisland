#include "gisland/module_lifecycle.hpp"

#include <algorithm>
#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gisland {
namespace {

constexpr auto failure_window = std::chrono::minutes{5};
constexpr std::size_t failure_limit = 10;

} // namespace

ModuleLifecycle::ModuleLifecycle(RestartPolicy policy, ModuleTimings timings)
    : policy_(policy), timings_(timings), next_backoff_(timings.initial_backoff) {}

ModuleState ModuleLifecycle::state() const noexcept { return state_; }

std::optional<MonotonicTime> ModuleLifecycle::restart_at() const noexcept { return restart_at_; }

std::optional<MonotonicTime> ModuleLifecycle::next_deadline() const noexcept {
  std::optional<MonotonicTime> deadline;
  for (const auto candidate : {handshake_deadline_, restart_at_, signal_at_}) {
    if (candidate.has_value() && (!deadline.has_value() || *candidate < *deadline)) {
      deadline = candidate;
    }
  }
  return deadline;
}

std::expected<StateTransition, LifecycleError> ModuleLifecycle::start(MonotonicTime now) {
  if (state_ != ModuleState::stopped && state_ != ModuleState::failed) {
    return std::unexpected(LifecycleError{state_, "start"});
  }

  if (state_ == ModuleState::failed) {
    failures_.clear();
  }
  next_backoff_ = timings_.initial_backoff;
  restart_at_.reset();
  pending_stop_cause_.reset();
  pending_signal_.reset();
  signal_at_.reset();
  handshake_deadline_ = now + timings_.handshake;
  return transition_to(ModuleState::starting, StopCause::requested, now);
}

std::expected<StateTransition, LifecycleError> ModuleLifecycle::ready(MonotonicTime now) {
  if (state_ != ModuleState::starting) {
    return std::unexpected(LifecycleError{state_, "ready"});
  }
  handshake_deadline_.reset();
  running_since_ = now;
  return transition_to(ModuleState::running, StopCause::requested, now);
}

std::vector<StateTransition> ModuleLifecycle::tick(MonotonicTime now) {
  if (state_ == ModuleState::starting && handshake_deadline_.has_value() &&
      now >= *handshake_deadline_) {
    handshake_deadline_.reset();
    const auto transition = transition_to(ModuleState::stopping, StopCause::handshake_timeout, now);
    begin_stopping(StopCause::handshake_timeout, now);
    return {transition};
  }

  if (state_ == ModuleState::backoff && restart_at_.has_value() && now >= *restart_at_) {
    restart_at_.reset();
    handshake_deadline_ = now + timings_.handshake;
    return {transition_to(ModuleState::starting, StopCause::requested, now)};
  }

  if (state_ == ModuleState::running) {
    reset_backoff_if_healthy(now);
  }
  return {};
}

std::vector<StateTransition> ModuleLifecycle::exited(StopCause cause, MonotonicTime now) {
  if (state_ != ModuleState::starting && state_ != ModuleState::running &&
      state_ != ModuleState::stopping) {
    return {};
  }

  reset_backoff_if_healthy(now);
  StopCause effective_cause = cause;
  if (state_ == ModuleState::stopping && pending_stop_cause_.has_value()) {
    effective_cause = *pending_stop_cause_;
  } else if (state_ == ModuleState::starting && cause == StopCause::clean_exit) {
    effective_cause = StopCause::failed_exit;
  }

  clear_process_state();
  if (is_failure(effective_cause) && record_failure(now)) {
    return {transition_to(ModuleState::failed, effective_cause, now)};
  }

  if (!should_restart(effective_cause)) {
    return {transition_to(ModuleState::stopped, effective_cause, now)};
  }

  const auto delay = is_failure(effective_cause) ? next_backoff_ : timings_.initial_backoff;
  restart_at_ = now + delay;
  if (is_failure(effective_cause)) {
    if (next_backoff_ >= timings_.maximum_backoff / 2) {
      next_backoff_ = timings_.maximum_backoff;
    } else {
      next_backoff_ = std::min(next_backoff_ * 2, timings_.maximum_backoff);
    }
  } else {
    next_backoff_ = timings_.initial_backoff;
  }
  return {transition_to(ModuleState::backoff, effective_cause, now)};
}

std::expected<StateTransition, LifecycleError> ModuleLifecycle::stop(MonotonicTime now) {
  if (state_ == ModuleState::starting || state_ == ModuleState::running) {
    const auto transition = transition_to(ModuleState::stopping, StopCause::requested, now);
    begin_stopping(StopCause::requested, now + timings_.graceful_shutdown);
    return transition;
  }
  if (state_ == ModuleState::backoff || state_ == ModuleState::failed) {
    restart_at_.reset();
    failures_.clear();
    next_backoff_ = timings_.initial_backoff;
    return transition_to(ModuleState::stopped, StopCause::requested, now);
  }
  return std::unexpected(LifecycleError{state_, "stop"});
}

std::expected<StateTransition, LifecycleError> ModuleLifecycle::fail(StopCause cause,
                                                                     MonotonicTime now) {
  if ((state_ != ModuleState::starting && state_ != ModuleState::running) || !is_failure(cause)) {
    return std::unexpected(LifecycleError{state_, "fail"});
  }
  const auto transition = transition_to(ModuleState::stopping, cause, now);
  begin_stopping(cause, now);
  return transition;
}

void ModuleLifecycle::reset_for_explicit_restart() {
  failures_.clear();
  next_backoff_ = timings_.initial_backoff;
}

std::optional<ShutdownSignal> ModuleLifecycle::due_signal(MonotonicTime now) const noexcept {
  if (state_ != ModuleState::stopping || !pending_signal_.has_value() || !signal_at_.has_value() ||
      now < *signal_at_) {
    return std::nullopt;
  }
  return pending_signal_;
}

std::expected<void, LifecycleError> ModuleLifecycle::signal_sent(ShutdownSignal signal,
                                                                 MonotonicTime now) {
  if (state_ != ModuleState::stopping || !pending_signal_.has_value() ||
      *pending_signal_ != signal || !signal_at_.has_value() || now < *signal_at_) {
    return std::unexpected(LifecycleError{state_, "signal_sent"});
  }

  if (signal == ShutdownSignal::terminate) {
    pending_signal_ = ShutdownSignal::kill;
    signal_at_ = now + timings_.terminate_grace;
  } else {
    pending_signal_.reset();
    signal_at_.reset();
  }
  return {};
}

StateTransition ModuleLifecycle::transition_to(ModuleState next, StopCause cause,
                                               MonotonicTime now) {
  const auto previous = std::exchange(state_, next);
  return StateTransition{previous, next, cause, now};
}

void ModuleLifecycle::begin_stopping(StopCause cause, MonotonicTime signal_at) {
  pending_stop_cause_ = cause;
  pending_signal_ = ShutdownSignal::terminate;
  signal_at_ = signal_at;
}

void ModuleLifecycle::clear_process_state() {
  handshake_deadline_.reset();
  running_since_.reset();
  pending_stop_cause_.reset();
  pending_signal_.reset();
  signal_at_.reset();
}

void ModuleLifecycle::reset_backoff_if_healthy(MonotonicTime now) {
  if (state_ == ModuleState::running && running_since_.has_value() &&
      now - *running_since_ >= timings_.healthy_reset) {
    next_backoff_ = timings_.initial_backoff;
  }
}

bool ModuleLifecycle::record_failure(MonotonicTime now) {
  const auto cutoff = now - failure_window;
  while (!failures_.empty() && failures_.front() < cutoff) {
    failures_.pop_front();
  }
  failures_.push_back(now);
  return failures_.size() >= failure_limit;
}

bool ModuleLifecycle::should_restart(StopCause cause) const noexcept {
  if (cause == StopCause::requested) {
    return false;
  }
  switch (policy_) {
  case RestartPolicy::always:
    return true;
  case RestartPolicy::on_failure:
    return is_failure(cause);
  case RestartPolicy::never:
    return false;
  }
  return false;
}

bool ModuleLifecycle::is_failure(StopCause cause) noexcept {
  return cause != StopCause::requested && cause != StopCause::clean_exit;
}

} // namespace gisland
