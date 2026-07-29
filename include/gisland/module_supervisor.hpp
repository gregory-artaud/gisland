#pragma once

#include "gisland/module_lifecycle.hpp"
#include "gisland/process_backend.hpp"
#include "gisland/protocol.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace gisland {

struct ModuleStartRequest {
  std::string instance_id;
  ProcessSpec process;
  InitMessage init;
  RestartPolicy restart{RestartPolicy::on_failure};
  ModuleTimings timings;
};

struct StateChangedEvent {
  std::string instance_id;
  StateTransition transition;
};

struct ProcessStartedEvent {
  std::string instance_id;
  pid_t pid;
  pid_t process_group;
  MonotonicTime at;
};

struct ModuleMessageEvent {
  std::string instance_id;
  ModuleMessage message;
  MonotonicTime at;
};

struct StderrLogEvent {
  std::string instance_id;
  std::string line;
  bool truncated;
  std::size_t repeated;
  MonotonicTime at;
};

struct ProtocolViolationEvent {
  std::string instance_id;
  ProtocolError error;
  std::size_t consecutive;
  std::size_t rolling;
  MonotonicTime at;
};

struct ContextsRemovedEvent {
  std::string instance_id;
  MonotonicTime at;
};

struct ProcessExitedEvent {
  std::string instance_id;
  pid_t pid;
  ExitStatus status;
  StopCause cause;
  MonotonicTime at;
};

struct SupervisorErrorEvent {
  std::string instance_id;
  std::string message;
  MonotonicTime at;
};

using SupervisorEvent = std::variant<StateChangedEvent, ProcessStartedEvent, ModuleMessageEvent,
                                     StderrLogEvent, ProtocolViolationEvent, ContextsRemovedEvent,
                                     ProcessExitedEvent, SupervisorErrorEvent>;

enum class SupervisorCommandError { invalid_request, queue_full, shutting_down };

class ModuleSupervisor {
public:
  static constexpr std::size_t maximum_instances = 32;

  ModuleSupervisor();
  ModuleSupervisor(const ModuleSupervisor &) = delete;
  ModuleSupervisor &operator=(const ModuleSupervisor &) = delete;
  ModuleSupervisor(ModuleSupervisor &&) = delete;
  ModuleSupervisor &operator=(ModuleSupervisor &&) = delete;
  ~ModuleSupervisor();

  [[nodiscard]] std::expected<void, SupervisorCommandError> start(ModuleStartRequest request);
  [[nodiscard]] std::expected<void, SupervisorCommandError> stop(std::string instance_id);
  [[nodiscard]] std::expected<void, SupervisorCommandError> restart(std::string instance_id);
  [[nodiscard]] std::expected<void, SupervisorCommandError> send(std::string instance_id,
                                                                 CoreMessage message);

  [[nodiscard]] std::vector<SupervisorEvent> drain_events();
  [[nodiscard]] std::vector<SupervisorEvent> wait_for_events(std::chrono::milliseconds timeout);
  void shutdown();

private:
  class Impl;
  std::unique_ptr<Impl> implementation_;
};

} // namespace gisland
