#include "gisland/module_supervisor.hpp"

#include "gisland/line_buffer.hpp"
#include "gisland/write_queue.hpp"

#include <csignal>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace gisland {
namespace {

[[nodiscard]] bool scene_uses_rich_content(const SceneNode &scene) {
  return std::visit(
      [](const auto &primitive) {
        using Primitive = std::decay_t<decltype(primitive)>;
        if constexpr (std::is_same_v<Primitive, RichText> ||
                      std::is_same_v<Primitive, ActionRegion>) {
          return true;
        } else if constexpr (std::is_same_v<Primitive, Row> || std::is_same_v<Primitive, Column>) {
          return std::ranges::any_of(primitive.children, [](const auto &child) {
            return scene_uses_rich_content(*child);
          });
        } else if constexpr (std::is_same_v<Primitive, Button>) {
          return scene_uses_rich_content(*primitive.content);
        }
        return false;
      },
      scene.value);
}

[[nodiscard]] bool scene_uses_indicator(const SceneNode &scene) {
  return std::visit(
      [](const auto &primitive) {
        using Primitive = std::decay_t<decltype(primitive)>;
        if constexpr (std::is_same_v<Primitive, Indicator>) {
          return true;
        } else if constexpr (std::is_same_v<Primitive, Row> || std::is_same_v<Primitive, Column>) {
          return std::ranges::any_of(
              primitive.children, [](const auto &child) { return scene_uses_indicator(*child); });
        } else if constexpr (std::is_same_v<Primitive, Button> ||
                             std::is_same_v<Primitive, ActionRegion>) {
          return scene_uses_indicator(*primitive.content);
        }
        return false;
      },
      scene.value);
}

[[nodiscard]] bool scene_uses_indicator_effects(const SceneNode &scene) {
  return std::visit(
      [](const auto &primitive) {
        using Primitive = std::decay_t<decltype(primitive)>;
        if constexpr (std::is_same_v<Primitive, Indicator>) {
          return !primitive.effects.empty();
        } else if constexpr (std::is_same_v<Primitive, Row> || std::is_same_v<Primitive, Column>) {
          return std::ranges::any_of(primitive.children, [](const auto &child) {
            return scene_uses_indicator_effects(*child);
          });
        } else if constexpr (std::is_same_v<Primitive, Button> ||
                             std::is_same_v<Primitive, ActionRegion>) {
          return scene_uses_indicator_effects(*primitive.content);
        }
        return false;
      },
      scene.value);
}

[[nodiscard]] std::optional<std::string> indicator_path(const PublishMessage &publish) {
  if (publish.compact && scene_uses_indicator(*publish.compact)) {
    return publish.independent_views ? "/views/compact" : "/compact";
  }
  if (publish.expanded && scene_uses_indicator(*publish.expanded)) {
    return publish.independent_views ? "/views/expanded" : "/expanded";
  }
  return std::nullopt;
}

[[nodiscard]] bool scene_uses_icon_role(const SceneNode &scene) {
  return std::visit(
      [](const auto &primitive) {
        using Primitive = std::decay_t<decltype(primitive)>;
        if constexpr (std::is_same_v<Primitive, Icon>) {
          return primitive.role != "body";
        } else if constexpr (std::is_same_v<Primitive, Row> || std::is_same_v<Primitive, Column>) {
          return std::ranges::any_of(
              primitive.children, [](const auto &child) { return scene_uses_icon_role(*child); });
        } else if constexpr (std::is_same_v<Primitive, Button> ||
                             std::is_same_v<Primitive, ActionRegion>) {
          return scene_uses_icon_role(*primitive.content);
        }
        return false;
      },
      scene.value);
}

[[nodiscard]] bool scene_uses_progress_transition(const SceneNode &scene) {
  return std::visit(
      [](const auto &primitive) {
        using Primitive = std::decay_t<decltype(primitive)>;
        if constexpr (std::is_same_v<Primitive, Progress>) {
          return primitive.transition_from.has_value();
        } else if constexpr (std::is_same_v<Primitive, Row> || std::is_same_v<Primitive, Column>) {
          return std::ranges::any_of(primitive.children, [](const auto &child) {
            return scene_uses_progress_transition(*child);
          });
        } else if constexpr (std::is_same_v<Primitive, Button> ||
                             std::is_same_v<Primitive, ActionRegion>) {
          return scene_uses_progress_transition(*primitive.content);
        }
        return false;
      },
      scene.value);
}

template <typename Predicate>
[[nodiscard]] std::optional<std::string> scene_feature_path(const PublishMessage &publish,
                                                            Predicate predicate) {
  if (publish.compact && predicate(*publish.compact)) {
    return publish.independent_views ? "/views/compact" : "/compact";
  }
  if (publish.expanded && predicate(*publish.expanded)) {
    return publish.independent_views ? "/views/expanded" : "/expanded";
  }
  return std::nullopt;
}

using namespace std::chrono_literals;

constexpr std::size_t command_capacity = 2048;
constexpr std::size_t event_capacity = 4096;
constexpr std::size_t violation_limit = 10;
constexpr std::size_t consecutive_violation_limit = 3;
constexpr auto violation_window = 60s;
constexpr auto maximum_poll_interval = 20ms;
constexpr std::size_t io_buffer_bytes = std::size_t{16} * 1024U;

struct StartCommand {
  ModuleStartRequest request;
};

struct StopCommand {
  std::string instance_id;
};

struct RestartCommand {
  std::string instance_id;
  std::uint64_t generation;
};

struct SendCommand {
  std::string instance_id;
  CoreMessage message;
};

struct SendActionCommand {
  std::string instance_id;
  std::uint64_t generation;
  ActionMessage message;
};

struct ReconfigureCommand {
  SupervisorReconfiguration reconfiguration;
};

struct ShutdownCommand {};

using Command = std::variant<StartCommand, StopCommand, RestartCommand, SendCommand,
                             SendActionCommand, ReconfigureCommand, ShutdownCommand>;

[[nodiscard]] StopCause stop_cause(const ExitStatus &status) {
  if (status.kind == ExitKind::signaled) {
    return StopCause::signal;
  }
  return status.code == 0 ? StopCause::clean_exit : StopCause::failed_exit;
}

[[nodiscard]] std::string line_buffer_message(LineBufferErrorCode code) {
  switch (code) {
  case LineBufferErrorCode::embedded_nul:
    return "protocol line contains an embedded NUL";
  case LineBufferErrorCode::line_too_long:
    return "protocol line exceeds the maximum byte count";
  case LineBufferErrorCode::finished:
    return "protocol stream received bytes after EOF";
  }
  return "protocol line buffering failed";
}

[[nodiscard]] bool is_active(ModuleState state) {
  return state == ModuleState::starting || state == ModuleState::running ||
         state == ModuleState::backoff || state == ModuleState::stopping;
}

struct Instance {
  explicit Instance(ModuleStartRequest start_request)
      : request(std::move(start_request)), lifecycle(request.restart, request.timings) {}

  ModuleStartRequest request;
  ModuleLifecycle lifecycle;
  std::optional<ProcessHandle> process;
  LineBuffer stdout_buffer{LineBuffer::protocol()};
  LineBuffer stderr_buffer{LineBuffer::standard_error()};
  WriteQueue writes;
  std::deque<MonotonicTime> violation_times;
  std::size_t consecutive_violations{0};
  bool ready{false};
  bool restart_after_stop{false};
  std::optional<std::uint64_t> restart_generation;
  std::optional<ModuleStartRequest> replacement_request;
  bool contexts_removed{true};
  bool stdout_eof{false};
  bool stderr_eof{false};
  std::optional<MonotonicTime> stdout_eof_at;
  std::optional<ProtocolVersion> negotiated_version;
  std::set<std::string> negotiated_capabilities;
  std::uint64_t generation{0};
};

} // namespace

class ModuleSupervisor::Impl {
public:
  Impl() {
    wake_descriptor_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_descriptor_ < 0) {
      throw std::system_error(errno, std::system_category(), "eventfd");
    }
    thread_ = std::jthread([this] { run_guarded(); });
  }

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  ~Impl() {
    shutdown();
    if (wake_descriptor_ >= 0) {
      static_cast<void>(::close(wake_descriptor_));
      wake_descriptor_ = -1;
    }
  }

  [[nodiscard]] std::expected<void, SupervisorCommandError> enqueue(Command command) {
    {
      const std::scoped_lock lock{command_mutex_};
      if (!accepting_commands_) {
        return std::unexpected(SupervisorCommandError::shutting_down);
      }
      if (commands_.size() >= command_capacity) {
        return std::unexpected(SupervisorCommandError::queue_full);
      }
      commands_.push_back(std::move(command));
    }
    wake();
    return {};
  }

  [[nodiscard]] std::vector<SupervisorEvent> drain_events() {
    const std::scoped_lock lock{event_mutex_};
    return take_events();
  }

  [[nodiscard]] std::vector<SupervisorEvent> wait_for_events(std::chrono::milliseconds timeout) {
    std::unique_lock lock{event_mutex_};
    static_cast<void>(
        event_condition_.wait_for(lock, timeout, [this] { return !events_.empty(); }));
    return take_events();
  }

  void shutdown() {
    const std::scoped_lock shutdown_lock{shutdown_mutex_};
    if (!thread_.joinable()) {
      return;
    }
    {
      const std::scoped_lock command_lock{command_mutex_};
      accepting_commands_ = false;
      commands_.emplace_back(ShutdownCommand{});
    }
    wake();
    thread_.join();
    event_condition_.notify_all();
  }

private:
  void run_guarded() noexcept {
    try {
      run();
    } catch (const std::exception &error) {
      report_thread_failure(error.what());
      instances_.clear();
    } catch (...) {
      report_thread_failure("unknown exception");
      instances_.clear();
    }
  }

  void report_thread_failure(std::string_view detail) noexcept {
    try {
      std::string message{"supervisor thread failed: "};
      message.append(detail);
      emit(SupervisorErrorEvent{"", std::move(message), std::chrono::steady_clock::now()});
    } catch (...) {
      constexpr std::string_view fallback = "gisland supervisor failure could not be queued\n";
      static_cast<void>(::write(STDERR_FILENO, fallback.data(), fallback.size()));
    }
  }

  void run() {
    while (true) {
      drain_wakeup();
      process_commands(std::chrono::steady_clock::now());
      auto now = std::chrono::steady_clock::now();
      advance_lifecycles(now);
      service_instances(now);
      reap_instances(now);

      if (global_shutdown_ && all_quiescent()) {
        break;
      }

      const auto interests = poll_interests();
      const auto ready = backend_.poll(interests, poll_timeout(std::chrono::steady_clock::now()));
      if (!ready.has_value()) {
        emit_error("", "poll failed: " + ready.error().message, std::chrono::steady_clock::now());
        throw std::runtime_error{"module poll failed"};
      }
    }
  }

  void wake() const noexcept {
    const std::uint64_t increment = 1;
    while (::write(wake_descriptor_, &increment, sizeof(increment)) < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
  }

  void drain_wakeup() const noexcept {
    std::uint64_t value = 0;
    while (::read(wake_descriptor_, &value, sizeof(value)) >= 0) {
    }
  }

  [[nodiscard]] std::deque<Command> take_commands() {
    std::deque<Command> commands;
    const std::scoped_lock lock{command_mutex_};
    commands.swap(commands_);
    return commands;
  }

  void process_commands(MonotonicTime now) {
    auto commands = take_commands();
    for (auto &command : commands) {
      std::visit(
          [this, now](auto &typed_command) {
            using Type = std::remove_cvref_t<decltype(typed_command)>;
            if constexpr (std::is_same_v<Type, StartCommand>) {
              handle_start(std::move(typed_command.request), now);
            } else if constexpr (std::is_same_v<Type, StopCommand>) {
              handle_stop(typed_command.instance_id, now, false);
            } else if constexpr (std::is_same_v<Type, RestartCommand>) {
              handle_restart(typed_command.instance_id, typed_command.generation, now);
            } else if constexpr (std::is_same_v<Type, SendCommand>) {
              handle_send(typed_command.instance_id, typed_command.message, now);
            } else if constexpr (std::is_same_v<Type, SendActionCommand>) {
              handle_send_action(typed_command, now);
            } else if constexpr (std::is_same_v<Type, ReconfigureCommand>) {
              handle_reconfigure(std::move(typed_command.reconfiguration), now);
            } else {
              begin_global_shutdown(now);
            }
          },
          command);
    }
  }

  [[nodiscard]] std::size_t active_instance_count() const {
    return static_cast<std::size_t>(std::ranges::count_if(
        instances_, [](const auto &entry) { return is_active(entry.second->lifecycle.state()); }));
  }

  void handle_start(ModuleStartRequest request, MonotonicTime now) {
    if (global_shutdown_) {
      emit_error(request.instance_id, "start rejected during supervisor shutdown", now);
      return;
    }
    auto existing = instances_.find(request.instance_id);
    if (existing != instances_.end()) {
      if (is_active(existing->second->lifecycle.state())) {
        emit_error(request.instance_id, "module instance is already active", now);
        return;
      }
      instances_.erase(existing);
    }
    if (active_instance_count() >= ModuleSupervisor::maximum_instances) {
      emit_error(request.instance_id, "the 32-instance supervision limit is reached", now);
      return;
    }

    const auto instance_id = request.instance_id;
    auto instance = std::make_unique<Instance>(std::move(request));
    const auto transition = instance->lifecycle.start(now);
    if (!transition.has_value()) {
      emit_error(instance_id, "module lifecycle rejected start", now);
      return;
    }
    auto [iterator, inserted] = instances_.emplace(instance_id, std::move(instance));
    if (!inserted) {
      emit_error(instance_id, "module instance insertion failed", now);
      return;
    }
    emit_transition(*iterator->second, *transition);
    spawn_instance(*iterator->second, now);
  }

  void handle_stop(const std::string &instance_id, MonotonicTime now, bool restart) {
    const auto iterator = instances_.find(instance_id);
    if (iterator == instances_.end()) {
      emit_error(instance_id, "stop requested for an unknown module instance", now);
      return;
    }
    auto &instance = *iterator->second;
    if (restart) {
      instance.restart_after_stop = true;
    }

    const auto state = instance.lifecycle.state();
    if (state == ModuleState::stopping) {
      return;
    }
    if (state == ModuleState::stopped) {
      if (restart && !global_shutdown_) {
        start_existing(instance, now);
      }
      return;
    }

    if (state == ModuleState::starting || state == ModuleState::running) {
      queue_shutdown(instance, restart ? "restart" : "requested-stop", now);
      emit_context_removal(instance, now);
    }
    const auto transition = instance.lifecycle.stop(now);
    if (!transition.has_value()) {
      emit_error(instance_id, "module lifecycle rejected stop", now);
      return;
    }
    emit_transition(instance, *transition);

    if (transition->to == ModuleState::stopped && instance.restart_after_stop &&
        !global_shutdown_) {
      instance.restart_after_stop = false;
      start_existing(instance, now);
    }
  }

  void handle_restart(const std::string &instance_id, std::uint64_t generation, MonotonicTime now) {
    const auto iterator = instances_.find(instance_id);
    if (iterator == instances_.end()) {
      emit_error(instance_id, "restart requested for an unknown module instance", now);
      emit(RestartCompletedEvent{instance_id, generation, false, ModuleState::failed, now});
      return;
    }
    auto &instance = *iterator->second;
    if (instance.restart_generation) {
      emit(RestartCompletedEvent{instance_id, generation, false, instance.lifecycle.state(), now});
      return;
    }
    instance.restart_generation = generation;
    instance.lifecycle.reset_for_explicit_restart();
    if (instance.lifecycle.state() == ModuleState::failed ||
        instance.lifecycle.state() == ModuleState::stopped) {
      start_existing(instance, now);
      return;
    }
    if (instance.lifecycle.state() == ModuleState::backoff) {
      const auto stopped = instance.lifecycle.stop(now);
      if (stopped.has_value()) {
        emit_transition(instance, *stopped);
      }
      start_existing(instance, now);
      return;
    }
    handle_stop(instance_id, now, true);
  }

  void handle_send(const std::string &instance_id, const CoreMessage &message, MonotonicTime now) {
    const auto iterator = instances_.find(instance_id);
    if (iterator == instances_.end()) {
      emit_error(instance_id, "message targeted an unknown module instance", now);
      return;
    }
    auto &instance = *iterator->second;
    if (instance.lifecycle.state() != ModuleState::running ||
        (!std::holds_alternative<ActionMessage>(message) &&
         !std::holds_alternative<VisibilityMessage>(message))) {
      emit_error(instance_id, "message is invalid in the module's current state", now);
      return;
    }
    if (!queue_message(instance, message, now)) {
      begin_failure(instance, StopCause::unresponsive, now);
    }
  }

  void handle_send_action(const SendActionCommand &command, MonotonicTime now) {
    const auto invocation_id = *command.message.invocation_id;
    const auto fail = [this, &command, invocation_id, now](ActionDeliveryError error) {
      emit(ActionDeliveryEvent{command.instance_id, command.generation, invocation_id, false, error,
                               now});
    };
    const auto iterator = instances_.find(command.instance_id);
    if (iterator == instances_.end()) {
      fail(ActionDeliveryError::unknown_instance);
      return;
    }
    auto &instance = *iterator->second;
    if (instance.generation != command.generation) {
      fail(ActionDeliveryError::generation_mismatch);
      return;
    }
    if (instance.lifecycle.state() != ModuleState::running || !instance.process.has_value()) {
      fail(ActionDeliveryError::unavailable);
      return;
    }

    try {
      auto record = serialize_core_message(CoreMessage{command.message});
      const auto queued = instance.writes.push(std::move(record));
      if (!queued.has_value()) {
        fail(ActionDeliveryError::queue_saturated);
        begin_failure(instance, StopCause::unresponsive, now);
        return;
      }
    } catch (const std::exception &) {
      fail(ActionDeliveryError::serialization_failed);
      return;
    }
    emit(ActionDeliveryEvent{command.instance_id, command.generation, invocation_id, true,
                             std::nullopt, now});
  }

  void cancel_restart(Instance &instance, MonotonicTime now) {
    instance.restart_after_stop = false;
    complete_restart(instance, false, instance.lifecycle.state(), now);
  }

  void start_replacement_if_quiescent(const std::string &instance_id, MonotonicTime now) {
    const auto existing = instances_.find(instance_id);
    if (existing == instances_.end() || existing->second->process.has_value() ||
        !existing->second->replacement_request ||
        (existing->second->lifecycle.state() != ModuleState::stopped &&
         existing->second->lifecycle.state() != ModuleState::failed)) {
      return;
    }
    // Presence is established immediately above before ownership moves to the new instance.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto request = std::move(*existing->second->replacement_request);
    instances_.erase(existing);
    handle_start(std::move(request), now);
  }

  void erase_pending_reconfiguration_start(const std::string &instance_id) {
    std::erase_if(pending_reconfiguration_starts_, [&instance_id](const auto &request) {
      return request.instance_id == instance_id;
    });
  }

  void queue_or_start_reconfiguration(ModuleStartRequest request, MonotonicTime now) {
    const auto pending = std::ranges::find(pending_reconfiguration_starts_, request.instance_id,
                                           &ModuleStartRequest::instance_id);
    if (pending != pending_reconfiguration_starts_.end()) {
      *pending = std::move(request);
      return;
    }
    if (active_instance_count() >= ModuleSupervisor::maximum_instances) {
      pending_reconfiguration_starts_.push_back(std::move(request));
      return;
    }
    handle_start(std::move(request), now);
  }

  void start_pending_reconfiguration(MonotonicTime now) {
    while (!global_shutdown_ && !pending_reconfiguration_starts_.empty() &&
           active_instance_count() < ModuleSupervisor::maximum_instances) {
      auto request = std::move(pending_reconfiguration_starts_.front());
      pending_reconfiguration_starts_.pop_front();
      handle_start(std::move(request), now);
    }
  }

  void handle_reconfigure(SupervisorReconfiguration reconfiguration, MonotonicTime now) {
    for (const auto &instance_id : reconfiguration.stop_instances) {
      erase_pending_reconfiguration_start(instance_id);
      const auto existing = instances_.find(instance_id);
      if (existing == instances_.end()) {
        continue;
      }
      cancel_restart(*existing->second, now);
      existing->second->replacement_request.reset();
      handle_stop(instance_id, now, false);
    }

    for (auto &request : reconfiguration.start_or_replace) {
      const std::string instance_id = request.instance_id;
      const auto existing = instances_.find(instance_id);
      if (existing == instances_.end()) {
        queue_or_start_reconfiguration(std::move(request), now);
        continue;
      }
      cancel_restart(*existing->second, now);
      existing->second->replacement_request = std::move(request);
      handle_stop(instance_id, now, false);
      start_replacement_if_quiescent(instance_id, now);
    }
  }

  void begin_global_shutdown(MonotonicTime now) {
    if (global_shutdown_) {
      return;
    }
    global_shutdown_ = true;
    pending_reconfiguration_starts_.clear();
    for (auto &[instance_id, instance] : instances_) {
      static_cast<void>(instance_id);
      handle_stop(instance->request.instance_id, now, false);
    }
  }

  void start_existing(Instance &instance, MonotonicTime now) {
    if (global_shutdown_) {
      complete_restart(instance, false, instance.lifecycle.state(), now);
      return;
    }
    if (active_instance_count() >= ModuleSupervisor::maximum_instances) {
      emit_error(instance.request.instance_id, "the 32-instance supervision limit is reached", now);
      complete_restart(instance, false, instance.lifecycle.state(), now);
      return;
    }
    const auto transition = instance.lifecycle.start(now);
    if (!transition.has_value()) {
      emit_error(instance.request.instance_id, "module lifecycle rejected restart", now);
      complete_restart(instance, false, instance.lifecycle.state(), now);
      return;
    }
    emit_transition(instance, *transition);
    spawn_instance(instance, now);
  }

  void spawn_instance(Instance &instance, MonotonicTime now) {
    instance.stdout_buffer = LineBuffer::protocol();
    instance.stderr_buffer = LineBuffer::standard_error();
    instance.writes = WriteQueue{};
    instance.consecutive_violations = 0;
    instance.ready = false;
    instance.contexts_removed = false;
    instance.stdout_eof = false;
    instance.stderr_eof = false;
    instance.stdout_eof_at.reset();
    instance.negotiated_version.reset();
    instance.negotiated_capabilities.clear();
    instance.generation = ++next_process_generation_;

    auto process = backend_.spawn(instance.request.process);
    if (!process.has_value()) {
      emit_error(instance.request.instance_id, "spawn failed: " + process.error().message, now);
      emit_context_removal(instance, now);
      const auto transitions = instance.lifecycle.exited(StopCause::spawn_error, now);
      emit_transitions(instance, transitions);
      return;
    }

    instance.process.emplace(std::move(*process));
    emit(ProcessStartedEvent{instance.request.instance_id, instance.process->pid(),
                             instance.process->process_group(), now, instance.generation});

    auto init = instance.request.init;
    init.instance_id = instance.request.instance_id;
    if (!queue_message(instance, CoreMessage{std::move(init)}, now)) {
      begin_failure(instance, StopCause::unresponsive, now);
    }
  }

  [[nodiscard]] bool queue_message(Instance &instance, const CoreMessage &message,
                                   MonotonicTime now) {
    try {
      auto record = serialize_core_message(message);
      const auto queued = instance.writes.push(std::move(record));
      if (!queued.has_value()) {
        emit_error(instance.request.instance_id, "outbound module queue is saturated", now);
        return false;
      }
      return true;
    } catch (const std::exception &error) {
      emit_error(instance.request.instance_id,
                 std::string{"core message serialization failed: "} + error.what(), now);
      return false;
    }
  }

  void queue_shutdown(Instance &instance, std::string reason, MonotonicTime now) {
    const CoreMessage message{
        ShutdownMessage{std::move(reason), instance.request.timings.graceful_shutdown}};
    if (!queue_message(instance, message, now)) {
      emit_error(instance.request.instance_id, "shutdown record could not enter the outbound queue",
                 now);
    }
  }

  void advance_lifecycles(MonotonicTime now) {
    for (auto &[instance_id, instance_pointer] : instances_) {
      static_cast<void>(instance_id);
      advance_lifecycle(*instance_pointer, now);
    }
  }

  void advance_lifecycle(Instance &instance, MonotonicTime now) {
    const auto transitions = instance.lifecycle.tick(now);
    for (const auto &transition : transitions) {
      if (transition.to == ModuleState::stopping) {
        emit_context_removal(instance, now);
      }
      emit_transition(instance, transition);
      if (transition.to == ModuleState::starting && !instance.process.has_value()) {
        spawn_instance(instance, now);
      }
    }
    send_due_signal(instance, now);
  }

  void send_due_signal(Instance &instance, MonotonicTime now) {
    if (!instance.process.has_value()) {
      return;
    }
    const auto signal = instance.lifecycle.due_signal(now);
    if (!signal.has_value()) {
      return;
    }

    const int native_signal = signal.value() == ShutdownSignal::terminate ? SIGTERM : SIGKILL;
    const auto signaled = backend_.signal_group(instance.process.value(), native_signal);
    if (!signaled.has_value()) {
      emit_error(instance.request.instance_id,
                 "process-group signal failed: " + signaled.error().message, now);
    }
    const auto recorded = instance.lifecycle.signal_sent(signal.value(), now);
    if (!recorded.has_value()) {
      emit_error(instance.request.instance_id, "module lifecycle rejected signal progress", now);
    }
  }

  void service_instances(MonotonicTime now) {
    for (auto &[instance_id, instance_pointer] : instances_) {
      static_cast<void>(instance_id);
      auto &instance = *instance_pointer;
      if (!instance.process.has_value()) {
        continue;
      }
      service_stdout(instance, now);
      service_stderr(instance, now);
      service_stdin(instance, now);
    }
  }

  void service_stdout(Instance &instance, MonotonicTime now) {
    if (instance.stdout_eof || !instance.process.has_value()) {
      return;
    }
    std::array<std::byte, io_buffer_bytes> buffer{};
    for (int read_attempt = 0; read_attempt < 64; ++read_attempt) {
      const auto read = backend_.read_stdout(*instance.process, buffer);
      if (!read.has_value()) {
        emit_error(instance.request.instance_id, "stdout read failed: " + read.error().message,
                   now);
        begin_failure(instance, StopCause::io_error, now);
        return;
      }
      if (read->transferred > 0) {
        if (!consume_stdout(instance, std::span<const std::byte>{buffer.data(), read->transferred},
                            now)) {
          return;
        }
      }
      if (read->eof) {
        finish_stdout(instance, now);
        return;
      }
      if (read->would_block || read->transferred == 0) {
        return;
      }
    }
  }

  [[nodiscard]] bool consume_stdout(Instance &instance, std::span<const std::byte> bytes,
                                    MonotonicTime now) {
    const auto lines = instance.stdout_buffer.append(bytes);
    if (!lines.has_value()) {
      record_violation(instance, ProtocolError{"", line_buffer_message(lines.error().code)}, now);
      return false;
    }
    for (const auto &line : lines.value()) {
      handle_protocol_line(instance, line.text, now);
      if (instance.lifecycle.state() == ModuleState::stopping) {
        return false;
      }
    }
    return true;
  }

  void finish_stdout(Instance &instance, MonotonicTime now) {
    instance.stdout_eof = true;
    instance.stdout_eof_at = now;
    auto final_line = instance.stdout_buffer.finish();
    if (!final_line.has_value()) {
      record_violation(instance, ProtocolError{"", line_buffer_message(final_line.error().code)},
                       now);
      return;
    }
    auto decoded = std::move(final_line).value();
    if (decoded.has_value()) {
      handle_protocol_line(instance, decoded.value().text, now);
    }
  }

  void service_stderr(Instance &instance, MonotonicTime now) {
    if (instance.stderr_eof || !instance.process.has_value()) {
      return;
    }
    std::array<std::byte, io_buffer_bytes> buffer{};
    for (int read_attempt = 0; read_attempt < 64; ++read_attempt) {
      const auto read = backend_.read_stderr(*instance.process, buffer);
      if (!read.has_value()) {
        emit_error(instance.request.instance_id, "stderr read failed: " + read.error().message,
                   now);
        begin_failure(instance, StopCause::io_error, now);
        return;
      }
      if (read->transferred > 0) {
        const auto lines = instance.stderr_buffer.append(
            std::span<const std::byte>{buffer.data(), read->transferred});
        if (!lines.has_value()) {
          emit_error(instance.request.instance_id, "stderr line buffering failed", now);
          return;
        }
        for (const auto &line : *lines) {
          emit_stderr(instance, line, now);
        }
      }
      if (read->eof) {
        instance.stderr_eof = true;
        auto final_line = instance.stderr_buffer.finish();
        if (!final_line.has_value()) {
          emit_error(instance.request.instance_id, "stderr final-line buffering failed", now);
          return;
        }
        auto decoded = std::move(final_line).value();
        if (decoded.has_value()) {
          emit_stderr(instance, decoded.value(), now);
        }
        return;
      }
      if (read->would_block || read->transferred == 0) {
        return;
      }
    }
  }

  void service_stdin(Instance &instance, MonotonicTime now) {
    if (!instance.process.has_value() || !instance.writes.wants_write()) {
      return;
    }
    for (int write_attempt = 0; write_attempt < 64 && instance.writes.wants_write();
         ++write_attempt) {
      const auto source = std::as_bytes(instance.writes.front_span());
      const auto written = backend_.write_stdin(*instance.process, source);
      if (!written.has_value()) {
        emit_error(instance.request.instance_id, "stdin write failed: " + written.error().message,
                   now);
        begin_failure(instance, StopCause::io_error, now);
        return;
      }
      if (written->transferred > 0) {
        const auto consumed = instance.writes.consume(written->transferred);
        if (!consumed.has_value()) {
          emit_error(instance.request.instance_id, "outbound queue accounting failed", now);
          begin_failure(instance, StopCause::io_error, now);
          return;
        }
      }
      if (written->eof) {
        begin_failure(instance, StopCause::io_error, now);
        return;
      }
      if (written->would_block || written->transferred == 0) {
        return;
      }
    }
  }

  void handle_reaped_instance(Instance &instance, std::vector<std::string> &replacements,
                              MonotonicTime now) {
    if (instance.replacement_request && instance.lifecycle.state() == ModuleState::stopped) {
      replacements.push_back(instance.request.instance_id);
      return;
    }
    if (instance.restart_after_stop && !global_shutdown_ &&
        instance.lifecycle.state() == ModuleState::stopped) {
      instance.restart_after_stop = false;
      start_existing(instance, now);
    }
    if (global_shutdown_ && instance.lifecycle.state() == ModuleState::backoff) {
      const auto stopped = instance.lifecycle.stop(now);
      if (stopped.has_value()) {
        emit_transition(instance, *stopped);
      }
    }
  }

  void reap_instances(MonotonicTime now) {
    std::vector<std::string> replacements;
    for (auto &[instance_id, instance_pointer] : instances_) {
      static_cast<void>(instance_id);
      auto &instance = *instance_pointer;
      if (!instance.process.has_value()) {
        continue;
      }
      const pid_t pid = instance.process->pid();
      const auto status = backend_.reap(*instance.process);
      if (!status.has_value()) {
        emit_error(instance.request.instance_id, "waitpid failed: " + status.error().message, now);
        begin_failure(instance, StopCause::io_error, now);
        continue;
      }
      if (!status->has_value()) {
        if (instance.stdout_eof_at.has_value() && now - *instance.stdout_eof_at >= 20ms &&
            (instance.lifecycle.state() == ModuleState::starting ||
             instance.lifecycle.state() == ModuleState::running)) {
          begin_failure(instance, StopCause::io_error, now);
        }
        continue;
      }

      emit_context_removal(instance, now);
      const ExitStatus exit_status = status.value().value();
      const auto transitions = instance.lifecycle.exited(stop_cause(exit_status), now);
      const auto effective_cause =
          transitions.empty() ? stop_cause(exit_status) : transitions.front().cause;
      emit(ProcessExitedEvent{instance.request.instance_id, pid, exit_status, effective_cause, now,
                              instance.generation});
      emit_transitions(instance, transitions);
      instance.process.reset();
      handle_reaped_instance(instance, replacements, now);
    }
    for (const auto &instance_id : replacements) {
      start_replacement_if_quiescent(instance_id, now);
    }
    start_pending_reconfiguration(now);
  }

  void handle_protocol_line(Instance &instance, const std::string &line, MonotonicTime now) {
    if (instance.lifecycle.state() == ModuleState::stopping) {
      return;
    }
    const auto message = parse_module_message(line);
    if (!message.has_value()) {
      record_violation(instance, message.error(), now);
      return;
    }

    if (!instance.ready) {
      const auto *ready = std::get_if<ReadyMessage>(&*message);
      if (ready == nullptr) {
        record_violation(instance, ProtocolError{"/type", "the first valid message must be ready"},
                         now);
        return;
      }
      if (!accept_ready(instance, *ready, now)) {
        return;
      }
      instance.consecutive_violations = 0;
      instance.ready = true;
      const auto transition = instance.lifecycle.ready(now);
      if (!transition.has_value()) {
        record_violation(instance, ProtocolError{"/type", "ready is invalid in this state"}, now);
        return;
      }
      emit_transition(instance, *transition);
      emit(ModuleMessageEvent{instance.request.instance_id, *message, now, instance.generation});
      return;
    }

    if (std::holds_alternative<ReadyMessage>(*message)) {
      record_violation(instance, ProtocolError{"/type", "a second ready is invalid"}, now);
      return;
    }
    if (instance.lifecycle.state() != ModuleState::running) {
      record_violation(instance, ProtocolError{"/type", "message is invalid in this state"}, now);
      return;
    }
    if (std::holds_alternative<DataMessage>(*message) &&
        !instance.negotiated_capabilities.contains("data-snapshots")) {
      record_violation(instance,
                       ProtocolError{"/type", "data-snapshots capability was not negotiated"}, now);
      return;
    }
    if (const auto *publish = std::get_if<PublishMessage>(&*message);
        publish != nullptr && !publish->resources.empty() &&
        !instance.negotiated_capabilities.contains("context-images")) {
      record_violation(instance,
                       ProtocolError{"/resources", "context-images capability was not negotiated"},
                       now);
      return;
    }
    if (const auto *publish = std::get_if<PublishMessage>(&*message);
        publish != nullptr &&
        ((publish->compact && scene_uses_rich_content(*publish->compact)) ||
         (publish->expanded && scene_uses_rich_content(*publish->expanded))) &&
        !instance.negotiated_capabilities.contains("rich-content")) {
      record_violation(
          instance, ProtocolError{"/compact", "rich-content capability was not negotiated"}, now);
      return;
    }
    if (const auto *publish = std::get_if<PublishMessage>(&*message);
        publish != nullptr && publish->independent_views &&
        !instance.negotiated_capabilities.contains("independent-views")) {
      record_violation(instance,
                       ProtocolError{"/views", "independent-views capability was not negotiated"},
                       now);
      return;
    }
    if (const auto *publish = std::get_if<PublishMessage>(&*message); publish != nullptr) {
      const auto path = indicator_path(*publish);
      if (path &&
          (!instance.negotiated_version || *instance.negotiated_version < ProtocolVersion{1, 6})) {
        record_violation(instance, ProtocolError{*path, "indicator requires protocol version 1.6"},
                         now);
        return;
      }
      if (path && !instance.negotiated_capabilities.contains("status-indicator")) {
        record_violation(
            instance, ProtocolError{*path, "status-indicator capability was not negotiated"}, now);
        return;
      }
      if (const auto effect_path = scene_feature_path(*publish, scene_uses_indicator_effects);
          effect_path) {
        if (!instance.negotiated_version || *instance.negotiated_version < ProtocolVersion{1, 9}) {
          record_violation(
              instance,
              ProtocolError{*effect_path, "indicator effects require protocol version 1.9"}, now);
          return;
        }
        if (!instance.negotiated_capabilities.contains("indicator-effects")) {
          record_violation(
              instance,
              ProtocolError{*effect_path, "indicator-effects capability was not negotiated"}, now);
          return;
        }
      }
      if (publish->presentation && publish->presentation->compact_style) {
        if (!instance.negotiated_version || *instance.negotiated_version < ProtocolVersion{1, 7}) {
          record_violation(instance,
                           ProtocolError{"/presentation/compact_style",
                                         "compact style requires protocol version 1.7"},
                           now);
          return;
        }
        if (!instance.negotiated_capabilities.contains("compact-view-styles")) {
          record_violation(instance,
                           ProtocolError{"/presentation/compact_style",
                                         "compact-view-styles capability was not negotiated"},
                           now);
          return;
        }
      }
      for (const auto &[feature_path, capability, feature_name] :
           std::array{std::tuple{scene_feature_path(*publish, scene_uses_icon_role),
                                 std::string_view{"icon-roles"}, std::string_view{"icon role"}},
                      std::tuple{scene_feature_path(*publish, scene_uses_progress_transition),
                                 std::string_view{"progress-transitions"},
                                 std::string_view{"progress transition"}}}) {
        if (!feature_path) {
          continue;
        }
        if (!instance.negotiated_version || *instance.negotiated_version < ProtocolVersion{1, 7}) {
          record_violation(instance,
                           ProtocolError{*feature_path, std::string{feature_name} +
                                                            " requires protocol version 1.7"},
                           now);
          return;
        }
        if (!instance.negotiated_capabilities.contains(std::string{capability})) {
          record_violation(instance,
                           ProtocolError{*feature_path, std::string{capability} +
                                                            " capability was not negotiated"},
                           now);
          return;
        }
      }
    }

    instance.consecutive_violations = 0;
    emit(ModuleMessageEvent{instance.request.instance_id, *message, now, instance.generation});
  }

  [[nodiscard]] bool accept_ready(Instance &instance, const ReadyMessage &ready,
                                  MonotonicTime now) {
    const ProtocolVersion selected{ready.protocol_major, ready.protocol_minor};
    if (selected < instance.request.init.minimum || selected > instance.request.init.maximum) {
      record_violation(instance,
                       ProtocolError{"/protocol_major", "protocol version is not supported"}, now);
      return false;
    }

    std::set<std::string> offered(instance.request.init.capabilities.begin(),
                                  instance.request.init.capabilities.end());
    for (const auto &capability : ready.capabilities) {
      if (!offered.contains(capability)) {
        record_violation(instance,
                         ProtocolError{"/capabilities", "capability was not offered by the core"},
                         now);
        return false;
      }
      if (capability == "context-images" && selected < ProtocolVersion{1, 2}) {
        record_violation(
            instance,
            ProtocolError{"/capabilities", "context-images requires protocol version 1.2"}, now);
        return false;
      }
      if (capability == "rich-content" && selected < ProtocolVersion{1, 3}) {
        record_violation(
            instance, ProtocolError{"/capabilities", "rich-content requires protocol version 1.3"},
            now);
        return false;
      }
      if (capability == "independent-views" && selected < ProtocolVersion{1, 4}) {
        record_violation(
            instance,
            ProtocolError{"/capabilities", "independent-views requires protocol version 1.4"}, now);
        return false;
      }
      if (capability == "ring-progress" && selected < ProtocolVersion{1, 5}) {
        record_violation(
            instance, ProtocolError{"/capabilities", "ring-progress requires protocol version 1.5"},
            now);
        return false;
      }
      if (capability == "status-indicator" && selected < ProtocolVersion{1, 6}) {
        record_violation(
            instance,
            ProtocolError{"/capabilities", "status-indicator requires protocol version 1.6"}, now);
        return false;
      }
      if ((capability == "compact-view-styles" || capability == "icon-roles" ||
           capability == "progress-transitions") &&
          selected < ProtocolVersion{1, 7}) {
        record_violation(
            instance, ProtocolError{"/capabilities", capability + " requires protocol version 1.7"},
            now);
        return false;
      }
      if (capability == "indicator-effects" && selected < ProtocolVersion{1, 9}) {
        record_violation(
            instance,
            ProtocolError{"/capabilities", "indicator-effects requires protocol version 1.9"}, now);
        return false;
      }
    }
    instance.negotiated_version = selected;
    instance.negotiated_capabilities =
        std::set<std::string>{ready.capabilities.begin(), ready.capabilities.end()};
    return true;
  }

  void record_violation(Instance &instance, ProtocolError error, MonotonicTime now) {
    if (instance.lifecycle.state() == ModuleState::stopping) {
      return;
    }
    const auto cutoff = now - violation_window;
    while (!instance.violation_times.empty() && instance.violation_times.front() < cutoff) {
      instance.violation_times.pop_front();
    }
    instance.violation_times.push_back(now);
    ++instance.consecutive_violations;
    const auto rolling = instance.violation_times.size();
    emit(ProtocolViolationEvent{instance.request.instance_id, std::move(error),
                                instance.consecutive_violations, rolling, now});

    if (instance.lifecycle.state() == ModuleState::starting ||
        instance.consecutive_violations >= consecutive_violation_limit ||
        rolling >= violation_limit) {
      begin_failure(instance, StopCause::protocol_violation, now);
    }
  }

  void begin_failure(Instance &instance, StopCause cause, MonotonicTime now) {
    if (instance.lifecycle.state() != ModuleState::starting &&
        instance.lifecycle.state() != ModuleState::running) {
      return;
    }
    emit_context_removal(instance, now);
    const auto transition = instance.lifecycle.fail(cause, now);
    if (transition.has_value()) {
      emit_transition(instance, *transition);
    } else {
      emit_error(instance.request.instance_id, "module lifecycle rejected failure", now);
    }
  }

  void emit_context_removal(Instance &instance, MonotonicTime now) {
    if (instance.contexts_removed) {
      return;
    }
    instance.contexts_removed = true;
    emit(ContextsRemovedEvent{instance.request.instance_id, now, instance.generation});
  }

  void emit_transition(Instance &instance, const StateTransition &transition) {
    emit(StateChangedEvent{instance.request.instance_id, transition});
    if (!instance.restart_generation) {
      return;
    }
    if (transition.to == ModuleState::running) {
      complete_restart(instance, true, transition.to, transition.at);
    } else if (transition.to == ModuleState::failed || (transition.from == ModuleState::starting &&
                                                        transition.cause != StopCause::requested &&
                                                        transition.to != ModuleState::running)) {
      complete_restart(instance, false, transition.to, transition.at);
    }
  }

  void emit_transitions(Instance &instance, const std::vector<StateTransition> &transitions) {
    for (const auto &transition : transitions) {
      emit_transition(instance, transition);
    }
  }

  void complete_restart(Instance &instance, bool succeeded, ModuleState state, MonotonicTime now) {
    if (!instance.restart_generation) {
      return;
    }
    const std::uint64_t generation = *instance.restart_generation;
    instance.restart_generation.reset();
    emit(RestartCompletedEvent{instance.request.instance_id, generation, succeeded, state, now});
  }

  void emit_stderr(const Instance &instance, const BufferedLine &line, MonotonicTime now) {
    emit(StderrLogEvent{instance.request.instance_id, line.text, line.truncated, 1, now});
  }

  void emit_error(std::string instance_id, std::string message, MonotonicTime now) {
    emit(SupervisorErrorEvent{std::move(instance_id), std::move(message), now});
  }

  void emit(SupervisorEvent event) {
    {
      const std::scoped_lock lock{event_mutex_};
      if (auto *incoming_log = std::get_if<StderrLogEvent>(&event)) {
        if (!events_.empty()) {
          if (auto *last_log = std::get_if<StderrLogEvent>(&events_.back());
              last_log != nullptr && last_log->instance_id == incoming_log->instance_id &&
              last_log->line == incoming_log->line &&
              last_log->truncated == incoming_log->truncated) {
            ++last_log->repeated;
            last_log->at = incoming_log->at;
            event_condition_.notify_all();
            return;
          }
        }
        if (events_.size() >= event_capacity) {
          const auto existing =
              std::ranges::find_if(events_, [incoming_log](const auto &candidate) {
                const auto *log = std::get_if<StderrLogEvent>(&candidate);
                return log != nullptr && log->instance_id == incoming_log->instance_id &&
                       log->line == incoming_log->line && log->truncated == incoming_log->truncated;
              });
          if (existing != events_.end()) {
            auto &log = std::get<StderrLogEvent>(*existing);
            ++log.repeated;
            log.at = incoming_log->at;
          }
          return;
        }
      } else if (events_.size() >= event_capacity) {
        const auto low_priority = std::ranges::find_if(events_, [](const auto &candidate) {
          return std::holds_alternative<StderrLogEvent>(candidate);
        });
        if (low_priority != events_.end()) {
          events_.erase(low_priority);
        }
      }
      events_.push_back(std::move(event));
    }
    event_condition_.notify_all();
  }

  [[nodiscard]] std::vector<SupervisorEvent> take_events() {
    std::vector<SupervisorEvent> result;
    result.reserve(events_.size());
    while (!events_.empty()) {
      result.push_back(std::move(events_.front()));
      events_.pop_front();
    }
    return result;
  }

  [[nodiscard]] std::vector<PollInterest> poll_interests() const {
    std::vector<PollInterest> interests;
    interests.reserve(1 + (instances_.size() * 3));
    std::size_t token = 0;
    interests.push_back(PollInterest{wake_descriptor_, true, false, token++});
    for (const auto &[instance_id, instance] : instances_) {
      static_cast<void>(instance_id);
      if (!instance->process.has_value()) {
        continue;
      }
      // Presence is established immediately above; ProcessHandle is intentionally non-copyable.
      const auto &process = *instance->process; // NOLINT(bugprone-unchecked-optional-access)
      if (process.stdout_fd() >= 0) {
        interests.push_back(PollInterest{process.stdout_fd(), true, false, token++});
      }
      if (process.stderr_fd() >= 0) {
        interests.push_back(PollInterest{process.stderr_fd(), true, false, token++});
      }
      if (process.stdin_fd() >= 0 && instance->writes.wants_write()) {
        interests.push_back(PollInterest{process.stdin_fd(), false, true, token++});
      }
    }
    return interests;
  }

  [[nodiscard]] std::chrono::milliseconds poll_timeout(MonotonicTime now) const {
    auto timeout = maximum_poll_interval;
    for (const auto &[instance_id, instance] : instances_) {
      static_cast<void>(instance_id);
      const auto deadline = instance->lifecycle.next_deadline();
      if (!deadline.has_value()) {
        continue;
      }
      if (*deadline <= now) {
        return 0ms;
      }
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
      if (remaining == 0ms) {
        remaining = 1ms;
      }
      timeout = std::min(timeout, remaining);
    }
    return timeout;
  }

  [[nodiscard]] bool all_quiescent() const {
    return std::ranges::all_of(instances_, [](const auto &entry) {
      const auto &instance = *entry.second;
      return !instance.process.has_value() && (instance.lifecycle.state() == ModuleState::stopped ||
                                               instance.lifecycle.state() == ModuleState::failed);
    });
  }

  ProcessBackend backend_;
  std::map<std::string, std::unique_ptr<Instance>> instances_;
  std::deque<ModuleStartRequest> pending_reconfiguration_starts_;
  int wake_descriptor_{-1};
  std::jthread thread_;
  bool global_shutdown_{false};
  std::uint64_t next_process_generation_{0};

  std::mutex command_mutex_;
  std::deque<Command> commands_;
  bool accepting_commands_{true};

  std::mutex event_mutex_;
  std::condition_variable event_condition_;
  std::deque<SupervisorEvent> events_;

  std::mutex shutdown_mutex_;
};

ModuleSupervisor::ModuleSupervisor() : implementation_(std::make_unique<Impl>()) {}

ModuleSupervisor::~ModuleSupervisor() = default;

std::expected<void, SupervisorCommandError> ModuleSupervisor::start(ModuleStartRequest request) {
  if (request.instance_id.empty() || request.process.argv.empty() ||
      request.process.argv.front().empty()) {
    return std::unexpected(SupervisorCommandError::invalid_request);
  }
  return implementation_->enqueue(StartCommand{std::move(request)});
}

std::expected<void, SupervisorCommandError> ModuleSupervisor::stop(std::string instance_id) {
  if (instance_id.empty()) {
    return std::unexpected(SupervisorCommandError::invalid_request);
  }
  return implementation_->enqueue(StopCommand{std::move(instance_id)});
}

std::expected<void, SupervisorCommandError> ModuleSupervisor::restart(std::string instance_id,
                                                                      std::uint64_t generation) {
  if (instance_id.empty()) {
    return std::unexpected(SupervisorCommandError::invalid_request);
  }
  return implementation_->enqueue(RestartCommand{std::move(instance_id), generation});
}

std::expected<void, SupervisorCommandError> ModuleSupervisor::send(std::string instance_id,
                                                                   CoreMessage message) {
  if (instance_id.empty()) {
    return std::unexpected(SupervisorCommandError::invalid_request);
  }
  return implementation_->enqueue(SendCommand{std::move(instance_id), std::move(message)});
}

std::expected<void, SupervisorCommandError> ModuleSupervisor::send_action(std::string instance_id,
                                                                          std::uint64_t generation,
                                                                          ActionMessage message) {
  if (instance_id.empty() || generation == 0 || message.action_id.empty() ||
      !message.invocation_id.has_value()) {
    return std::unexpected(SupervisorCommandError::invalid_request);
  }
  return implementation_->enqueue(
      SendActionCommand{std::move(instance_id), generation, std::move(message)});
}

std::expected<void, SupervisorCommandError>
ModuleSupervisor::reconfigure(SupervisorReconfiguration reconfiguration) {
  std::set<std::string, std::less<>> instance_ids;
  for (const auto &instance_id : reconfiguration.stop_instances) {
    if (instance_id.empty() || !instance_ids.insert(instance_id).second) {
      return std::unexpected(SupervisorCommandError::invalid_request);
    }
  }
  for (const auto &request : reconfiguration.start_or_replace) {
    if (request.instance_id.empty() || request.process.argv.empty() ||
        request.process.argv.front().empty() || !instance_ids.insert(request.instance_id).second) {
      return std::unexpected(SupervisorCommandError::invalid_request);
    }
  }
  return implementation_->enqueue(ReconfigureCommand{std::move(reconfiguration)});
}

std::vector<SupervisorEvent> ModuleSupervisor::drain_events() {
  return implementation_->drain_events();
}

std::vector<SupervisorEvent> ModuleSupervisor::wait_for_events(std::chrono::milliseconds timeout) {
  return implementation_->wait_for_events(timeout);
}

void ModuleSupervisor::shutdown() { implementation_->shutdown(); }

} // namespace gisland
