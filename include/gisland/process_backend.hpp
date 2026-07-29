#pragma once

#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gisland {

struct ProcessSpec {
  std::vector<std::string> argv;
  std::map<std::string, std::string> environment;
  std::optional<std::filesystem::path> working_directory;
};

struct ProcessError {
  std::string operation;
  int code;
  std::string message;
};

struct IoResult {
  std::size_t transferred;
  bool eof;
  bool would_block;
};

enum class ExitKind { exited, signaled };

struct ExitStatus {
  ExitKind kind;
  int code;

  [[nodiscard]] bool success() const noexcept {
    return kind == ExitKind::exited && code == 0;
  }
};

struct PollInterest {
  int descriptor;
  bool read;
  bool write;
  std::size_t token;
};

struct PollReady {
  std::size_t token;
  bool readable;
  bool writable;
  bool hangup;
  bool error;
};

class ProcessBackend;

class ProcessHandle {
public:
  ProcessHandle(const ProcessHandle &) = delete;
  ProcessHandle &operator=(const ProcessHandle &) = delete;
  ProcessHandle(ProcessHandle &&other) noexcept;
  ProcessHandle &operator=(ProcessHandle &&other) noexcept;
  ~ProcessHandle();

  [[nodiscard]] pid_t pid() const noexcept;
  [[nodiscard]] pid_t process_group() const noexcept;
  [[nodiscard]] int stdin_fd() const noexcept;
  [[nodiscard]] int stdout_fd() const noexcept;
  [[nodiscard]] int stderr_fd() const noexcept;

private:
  friend class ProcessBackend;

  ProcessHandle(pid_t pid, int stdin_descriptor, int stdout_descriptor, int stderr_descriptor);
  void cleanup() noexcept;
  static void close_descriptor(int &descriptor) noexcept;

  pid_t pid_{-1};
  pid_t process_group_{-1};
  int stdin_descriptor_{-1};
  int stdout_descriptor_{-1};
  int stderr_descriptor_{-1};
};

class ProcessBackend {
public:
  ProcessBackend();

  [[nodiscard]] std::expected<ProcessHandle, ProcessError> spawn(const ProcessSpec &spec) const;
  [[nodiscard]] std::expected<IoResult, ProcessError>
  read_stdout(ProcessHandle &process, std::span<std::byte> destination) const;
  [[nodiscard]] std::expected<IoResult, ProcessError>
  read_stderr(ProcessHandle &process, std::span<std::byte> destination) const;
  [[nodiscard]] std::expected<IoResult, ProcessError>
  write_stdin(ProcessHandle &process, std::span<const std::byte> source) const;
  [[nodiscard]] std::expected<void, ProcessError> close_stdin(ProcessHandle &process) const;
  [[nodiscard]] std::expected<void, ProcessError> signal_group(const ProcessHandle &process,
                                                               int signal) const;
  [[nodiscard]] std::expected<std::vector<PollReady>, ProcessError>
  poll(std::span<const PollInterest> interests, std::chrono::milliseconds timeout) const;
  [[nodiscard]] std::expected<std::optional<ExitStatus>, ProcessError>
  reap(ProcessHandle &process) const;

private:
  [[nodiscard]] static std::expected<IoResult, ProcessError>
  read_descriptor(int &descriptor, std::span<std::byte> destination, std::string operation);

  std::optional<ProcessError> initialization_error_;
};

} // namespace gisland
