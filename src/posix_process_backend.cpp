#include "gisland/process_backend.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

extern char **environ;

namespace gisland {
namespace {

[[nodiscard]] ProcessError process_error(std::string operation, int code) {
  return ProcessError{std::move(operation), code, std::system_category().message(code)};
}

class PipeSet {
public:
  PipeSet() = default;
  PipeSet(const PipeSet &) = delete;
  PipeSet &operator=(const PipeSet &) = delete;
  ~PipeSet() {
    for (auto &descriptor : descriptors_) {
      close_descriptor(descriptor);
    }
  }

  [[nodiscard]] std::expected<void, ProcessError> create() {
    for (std::size_t index = 0; index < descriptors_.size(); index += 2) {
      std::array<int, 2> pipe_descriptors{};
      if (::pipe2(pipe_descriptors.data(), O_CLOEXEC) != 0) {
        return std::unexpected(process_error("pipe2", errno));
      }
      descriptors_[index] = pipe_descriptors[0];
      descriptors_[index + 1] = pipe_descriptors[1];
    }
    return {};
  }

  [[nodiscard]] int child_stdin() const { return descriptors_[0]; }
  [[nodiscard]] int parent_stdin() const { return descriptors_[1]; }
  [[nodiscard]] int parent_stdout() const { return descriptors_[2]; }
  [[nodiscard]] int child_stdout() const { return descriptors_[3]; }
  [[nodiscard]] int parent_stderr() const { return descriptors_[4]; }
  [[nodiscard]] int child_stderr() const { return descriptors_[5]; }
  [[nodiscard]] const std::array<int, 6> &all() const { return descriptors_; }

  [[nodiscard]] int take_parent_stdin() { return take(1); }
  [[nodiscard]] int take_parent_stdout() { return take(2); }
  [[nodiscard]] int take_parent_stderr() { return take(4); }

private:
  static void close_descriptor(int &descriptor) {
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
      descriptor = -1;
    }
  }

  [[nodiscard]] int take(std::size_t index) { return std::exchange(descriptors_[index], -1); }

  std::array<int, 6> descriptors_{-1, -1, -1, -1, -1, -1};
};

[[nodiscard]] std::expected<void, ProcessError> make_nonblocking(int descriptor) {
  const int flags = ::fcntl(descriptor, F_GETFL);
  if (flags < 0) {
    return std::unexpected(process_error("fcntl(F_GETFL)", errno));
  }
  if (::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
    return std::unexpected(process_error("fcntl(F_SETFL)", errno));
  }
  return {};
}

[[nodiscard]] std::expected<void, ProcessError> add_file_action(int result, std::string operation) {
  if (result != 0) {
    return std::unexpected(process_error(std::move(operation), result));
  }
  return {};
}

[[nodiscard]] std::expected<void, ProcessError>
configure_file_actions(posix_spawn_file_actions_t &actions, const PipeSet &pipes,
                       const std::optional<std::filesystem::path> &working_directory) {
  if (working_directory.has_value()) {
    const auto directory = working_directory->string();
    auto result =
        add_file_action(::posix_spawn_file_actions_addchdir_np(&actions, directory.c_str()),
                        "posix_spawn_file_actions_addchdir_np");
    if (!result.has_value()) {
      return result;
    }
  }

  for (const auto &[source, destination] : {std::pair{pipes.child_stdin(), STDIN_FILENO},
                                            std::pair{pipes.child_stdout(), STDOUT_FILENO},
                                            std::pair{pipes.child_stderr(), STDERR_FILENO}}) {
    auto result = add_file_action(::posix_spawn_file_actions_adddup2(&actions, source, destination),
                                  "posix_spawn_file_actions_adddup2");
    if (!result.has_value()) {
      return result;
    }
  }
  for (const int descriptor : pipes.all()) {
    auto result = add_file_action(::posix_spawn_file_actions_addclose(&actions, descriptor),
                                  "posix_spawn_file_actions_addclose");
    if (!result.has_value()) {
      return result;
    }
  }
  return {};
}

[[nodiscard]] std::expected<void, ProcessError>
configure_attributes(posix_spawnattr_t &attributes) {
  sigset_t defaults;
  if (::sigemptyset(&defaults) != 0 || ::sigaddset(&defaults, SIGPIPE) != 0) {
    return std::unexpected(process_error("sigset", errno));
  }

  const short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF;
  int result = ::posix_spawnattr_setflags(&attributes, flags);
  if (result != 0) {
    return std::unexpected(process_error("posix_spawnattr_setflags", result));
  }
  result = ::posix_spawnattr_setpgroup(&attributes, 0);
  if (result != 0) {
    return std::unexpected(process_error("posix_spawnattr_setpgroup", result));
  }
  result = ::posix_spawnattr_setsigdefault(&attributes, &defaults);
  if (result != 0) {
    return std::unexpected(process_error("posix_spawnattr_setsigdefault", result));
  }
  return {};
}

[[nodiscard]] bool contains_nul(const std::string &value) {
  return value.find('\0') != std::string::npos;
}

[[nodiscard]] std::expected<void, ProcessError> validate_spec(const ProcessSpec &spec) {
  if (spec.argv.empty() || spec.argv.front().empty()) {
    return std::unexpected(process_error("argv", EINVAL));
  }
  for (const auto &argument : spec.argv) {
    if (contains_nul(argument)) {
      return std::unexpected(process_error("argv", EINVAL));
    }
  }
  for (const auto &[key, value] : spec.environment) {
    if (key.empty() || key.contains('=') || contains_nul(key) || contains_nul(value)) {
      return std::unexpected(process_error("environment", EINVAL));
    }
  }
  if (spec.working_directory.has_value()) {
    const auto directory = spec.working_directory->string();
    if (!spec.working_directory->is_absolute() || contains_nul(directory)) {
      return std::unexpected(process_error("working_directory", EINVAL));
    }
  }
  return {};
}

[[nodiscard]] std::vector<std::string>
build_environment(const std::map<std::string, std::string> &overrides) {
  std::map<std::string, std::string> values;
  for (char **entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string value{*entry};
    const auto separator = value.find('=');
    if (separator != std::string::npos) {
      values.insert_or_assign(value.substr(0, separator), value.substr(separator + 1));
    }
  }
  for (const auto &[key, value] : overrides) {
    values.insert_or_assign(key, value);
  }

  std::vector<std::string> environment;
  environment.reserve(values.size());
  for (const auto &[key, value] : values) {
    environment.push_back(key + '=' + value);
  }
  return environment;
}

[[nodiscard]] std::vector<char *> mutable_pointers(std::vector<std::string> &values) {
  std::vector<char *> pointers;
  pointers.reserve(values.size() + 1);
  for (auto &value : values) {
    pointers.push_back(value.data());
  }
  pointers.push_back(nullptr);
  return pointers;
}

} // namespace

ProcessHandle::ProcessHandle(pid_t pid, int stdin_descriptor, int stdout_descriptor,
                             int stderr_descriptor)
    : pid_(pid), process_group_(pid), stdin_descriptor_(stdin_descriptor),
      stdout_descriptor_(stdout_descriptor), stderr_descriptor_(stderr_descriptor) {}

ProcessHandle::ProcessHandle(ProcessHandle &&other) noexcept
    : pid_(std::exchange(other.pid_, -1)), process_group_(std::exchange(other.process_group_, -1)),
      stdin_descriptor_(std::exchange(other.stdin_descriptor_, -1)),
      stdout_descriptor_(std::exchange(other.stdout_descriptor_, -1)),
      stderr_descriptor_(std::exchange(other.stderr_descriptor_, -1)) {}

ProcessHandle &ProcessHandle::operator=(ProcessHandle &&other) noexcept {
  if (this != &other) {
    cleanup();
    pid_ = std::exchange(other.pid_, -1);
    process_group_ = std::exchange(other.process_group_, -1);
    stdin_descriptor_ = std::exchange(other.stdin_descriptor_, -1);
    stdout_descriptor_ = std::exchange(other.stdout_descriptor_, -1);
    stderr_descriptor_ = std::exchange(other.stderr_descriptor_, -1);
  }
  return *this;
}

ProcessHandle::~ProcessHandle() { cleanup(); }

pid_t ProcessHandle::pid() const noexcept { return pid_; }

pid_t ProcessHandle::process_group() const noexcept { return process_group_; }

int ProcessHandle::stdin_fd() const noexcept { return stdin_descriptor_; }

int ProcessHandle::stdout_fd() const noexcept { return stdout_descriptor_; }

int ProcessHandle::stderr_fd() const noexcept { return stderr_descriptor_; }

void ProcessHandle::cleanup() noexcept {
  close_descriptor(stdin_descriptor_);
  close_descriptor(stdout_descriptor_);
  close_descriptor(stderr_descriptor_);
  if (process_group_ > 0) {
    static_cast<void>(::kill(-process_group_, SIGKILL));
  }
  if (pid_ > 0) {
    int status = 0;
    while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
    }
  }
  pid_ = -1;
  process_group_ = -1;
}

void ProcessHandle::close_descriptor(int &descriptor) noexcept {
  if (descriptor >= 0) {
    while (::close(descriptor) < 0 && errno == EINTR) {
    }
    descriptor = -1;
  }
}

ProcessBackend::ProcessBackend() {
  struct sigaction action{};
  action.sa_handler = SIG_IGN;
  if (::sigemptyset(&action.sa_mask) != 0 || ::sigaction(SIGPIPE, &action, nullptr) != 0) {
    initialization_error_ = process_error("sigaction(SIGPIPE)", errno);
  }
}

std::expected<ProcessHandle, ProcessError> ProcessBackend::spawn(const ProcessSpec &spec) const {
  if (initialization_error_.has_value()) {
    return std::unexpected(*initialization_error_);
  }
  auto validation = validate_spec(spec);
  if (!validation.has_value()) {
    return std::unexpected(validation.error());
  }

  PipeSet pipes;
  auto created = pipes.create();
  if (!created.has_value()) {
    return std::unexpected(created.error());
  }
  for (const int descriptor :
       {pipes.parent_stdin(), pipes.parent_stdout(), pipes.parent_stderr()}) {
    auto nonblocking = make_nonblocking(descriptor);
    if (!nonblocking.has_value()) {
      return std::unexpected(nonblocking.error());
    }
  }

  posix_spawn_file_actions_t actions;
  int result = ::posix_spawn_file_actions_init(&actions);
  if (result != 0) {
    return std::unexpected(process_error("posix_spawn_file_actions_init", result));
  }
  const auto destroy_actions = [&actions]() { ::posix_spawn_file_actions_destroy(&actions); };
  auto configured_actions = configure_file_actions(actions, pipes, spec.working_directory);
  if (!configured_actions.has_value()) {
    destroy_actions();
    return std::unexpected(configured_actions.error());
  }

  posix_spawnattr_t attributes;
  result = ::posix_spawnattr_init(&attributes);
  if (result != 0) {
    destroy_actions();
    return std::unexpected(process_error("posix_spawnattr_init", result));
  }
  const auto destroy_attributes = [&attributes]() { ::posix_spawnattr_destroy(&attributes); };
  auto configured_attributes = configure_attributes(attributes);
  if (!configured_attributes.has_value()) {
    destroy_attributes();
    destroy_actions();
    return std::unexpected(configured_attributes.error());
  }

  auto arguments = spec.argv;
  auto argument_pointers = mutable_pointers(arguments);
  auto environment = build_environment(spec.environment);
  auto environment_pointers = mutable_pointers(environment);
  pid_t pid = -1;
  result = ::posix_spawnp(&pid, argument_pointers.front(), &actions, &attributes,
                          argument_pointers.data(), environment_pointers.data());
  destroy_attributes();
  destroy_actions();
  if (result != 0) {
    return std::unexpected(process_error("posix_spawnp", result));
  }

  return ProcessHandle{pid, pipes.take_parent_stdin(), pipes.take_parent_stdout(),
                       pipes.take_parent_stderr()};
}

std::expected<IoResult, ProcessError>
ProcessBackend::read_stdout(ProcessHandle &process, std::span<std::byte> destination) const {
  return read_descriptor(process.stdout_descriptor_, destination, "read(stdout)");
}

std::expected<IoResult, ProcessError>
ProcessBackend::read_stderr(ProcessHandle &process, std::span<std::byte> destination) const {
  return read_descriptor(process.stderr_descriptor_, destination, "read(stderr)");
}

std::expected<IoResult, ProcessError>
ProcessBackend::write_stdin(ProcessHandle &process, std::span<const std::byte> source) const {
  if (process.stdin_descriptor_ < 0) {
    return IoResult{0, true, false};
  }
  while (true) {
    const auto written =
        ::write(process.stdin_descriptor_, static_cast<const void *>(source.data()), source.size());
    if (written >= 0) {
      return IoResult{static_cast<std::size_t>(written), false, false};
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return IoResult{0, false, true};
    }
    if (errno == EPIPE) {
      ProcessHandle::close_descriptor(process.stdin_descriptor_);
      return IoResult{0, true, false};
    }
    return std::unexpected(process_error("write(stdin)", errno));
  }
}

std::expected<void, ProcessError> ProcessBackend::close_stdin(ProcessHandle &process) const {
  ProcessHandle::close_descriptor(process.stdin_descriptor_);
  return {};
}

std::expected<void, ProcessError> ProcessBackend::signal_group(const ProcessHandle &process,
                                                               int signal) const {
  if (process.process_group_ <= 0) {
    return std::unexpected(process_error("kill(process_group)", ESRCH));
  }
  if (::kill(-process.process_group_, signal) != 0 && errno != ESRCH) {
    return std::unexpected(process_error("kill(process_group)", errno));
  }
  return {};
}

std::expected<std::vector<PollReady>, ProcessError>
ProcessBackend::poll(std::span<const PollInterest> interests,
                     std::chrono::milliseconds timeout) const {
  std::vector<struct pollfd> descriptors;
  descriptors.reserve(interests.size());
  for (const auto &interest : interests) {
    short events = 0;
    if (interest.read) {
      events = static_cast<short>(events | POLLIN);
    }
    if (interest.write) {
      events = static_cast<short>(events | POLLOUT);
    }
    descriptors.push_back(pollfd{interest.descriptor, events, 0});
  }

  const auto clamped_timeout = std::clamp(
      timeout.count(), std::int64_t{0}, static_cast<std::int64_t>(std::numeric_limits<int>::max()));
  int result = 0;
  do {
    result = ::poll(descriptors.data(), descriptors.size(), static_cast<int>(clamped_timeout));
  } while (result < 0 && errno == EINTR);
  if (result < 0) {
    return std::unexpected(process_error("poll", errno));
  }

  std::vector<PollReady> ready;
  ready.reserve(static_cast<std::size_t>(result));
  for (std::size_t index = 0; index < descriptors.size(); ++index) {
    const auto events = descriptors[index].revents;
    if (events == 0) {
      continue;
    }
    ready.push_back(PollReady{
        interests[index].token,
        (events & POLLIN) != 0,
        (events & POLLOUT) != 0,
        (events & POLLHUP) != 0,
        (events & (POLLERR | POLLNVAL)) != 0,
    });
  }
  return ready;
}

std::expected<std::optional<ExitStatus>, ProcessError>
ProcessBackend::reap(ProcessHandle &process) const {
  if (process.pid_ <= 0) {
    return std::unexpected(process_error("waitpid", ECHILD));
  }
  int status = 0;
  pid_t result = -1;
  do {
    result = ::waitpid(process.pid_, &status, WNOHANG);
  } while (result < 0 && errno == EINTR);
  if (result == 0) {
    return std::optional<ExitStatus>{};
  }
  if (result < 0) {
    return std::unexpected(process_error("waitpid", errno));
  }

  if (process.process_group_ > 0) {
    static_cast<void>(::kill(-process.process_group_, SIGKILL));
  }
  process.pid_ = -1;
  process.process_group_ = -1;
  if (WIFEXITED(status)) {
    return std::optional<ExitStatus>{ExitStatus{ExitKind::exited, WEXITSTATUS(status)}};
  }
  if (WIFSIGNALED(status)) {
    return std::optional<ExitStatus>{ExitStatus{ExitKind::signaled, WTERMSIG(status)}};
  }
  return std::unexpected(process_error("waitpid(status)", ECHILD));
}

std::expected<IoResult, ProcessError>
ProcessBackend::read_descriptor(int &descriptor, std::span<std::byte> destination,
                                std::string operation) {
  if (descriptor < 0) {
    return IoResult{0, true, false};
  }
  while (true) {
    const auto read_count =
        ::read(descriptor, static_cast<void *>(destination.data()), destination.size());
    if (read_count > 0) {
      return IoResult{static_cast<std::size_t>(read_count), false, false};
    }
    if (read_count == 0) {
      ProcessHandle::close_descriptor(descriptor);
      return IoResult{0, true, false};
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return IoResult{0, false, true};
    }
    return std::unexpected(process_error(std::move(operation), errno));
  }
}

} // namespace gisland
