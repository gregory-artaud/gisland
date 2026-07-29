#include "gisland/process_backend.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef GISLAND_FAKE_MODULE_PATH
#error "GISLAND_FAKE_MODULE_PATH must name the integration-test helper"
#endif

namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::span<const std::byte> bytes(std::string_view value) {
  return std::as_bytes(std::span{value.data(), value.size()});
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "gisland-process-XXXXXX").string();
    std::vector<char> writable_pattern(pattern.begin(), pattern.end());
    writable_pattern.push_back('\0');
    const char *created = ::mkdtemp(writable_pattern.data());
    REQUIRE(created != nullptr);
    path_ = created;
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

[[nodiscard]] std::string read_stdout_line(gisland::ProcessBackend &backend,
                                           gisland::ProcessHandle &process) {
  std::string output;
  std::array<std::byte, 4096> buffer{};
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto read = backend.read_stdout(process, buffer);
    REQUIRE(read.has_value());
    if (read->transferred > 0) {
      output.append(reinterpret_cast<const char *>(buffer.data()), read->transferred);
      const auto newline = output.find('\n');
      if (newline != std::string::npos) {
        return output.substr(0, newline);
      }
    }
    if (read->eof) {
      break;
    }
    const std::array interests{
        gisland::PollInterest{process.stdout_fd(), true, false, 1},
    };
    REQUIRE(backend.poll(interests, 50ms).has_value());
  }
  FAIL("timed out waiting for child stdout");
  return {};
}

[[nodiscard]] std::string read_stderr_to_eof(gisland::ProcessBackend &backend,
                                             gisland::ProcessHandle &process) {
  std::string output;
  std::array<std::byte, 4096> buffer{};
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto read = backend.read_stderr(process, buffer);
    REQUIRE(read.has_value());
    if (read->transferred > 0) {
      output.append(reinterpret_cast<const char *>(buffer.data()), read->transferred);
    }
    if (read->eof) {
      return output;
    }
    const std::array interests{
        gisland::PollInterest{process.stderr_fd(), true, false, 2},
    };
    REQUIRE(backend.poll(interests, 50ms).has_value());
  }
  FAIL("timed out waiting for child stderr EOF");
  return {};
}

void write_all(gisland::ProcessBackend &backend, gisland::ProcessHandle &process,
               std::string_view value) {
  std::size_t offset = 0;
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (offset < value.size() && std::chrono::steady_clock::now() < deadline) {
    const auto write = backend.write_stdin(process, bytes(value.substr(offset)));
    REQUIRE(write.has_value());
    REQUIRE_FALSE(write->eof);
    offset += write->transferred;
    if (write->would_block) {
      const std::array interests{
          gisland::PollInterest{process.stdin_fd(), false, true, 3},
      };
      REQUIRE(backend.poll(interests, 50ms).has_value());
    }
  }
  REQUIRE(offset == value.size());
}

[[nodiscard]] gisland::ExitStatus wait_for_exit(gisland::ProcessBackend &backend,
                                                gisland::ProcessHandle &process) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto status = backend.reap(process);
    REQUIRE(status.has_value());
    if (status->has_value()) {
      return status->value_or(gisland::ExitStatus{gisland::ExitKind::exited, -1});
    }
    REQUIRE(backend.poll({}, 10ms).has_value());
  }
  FAIL("timed out waiting for child exit");
  return gisland::ExitStatus{gisland::ExitKind::exited, -1};
}

[[nodiscard]] gisland::ProcessSpec fake_spec(std::string mode) {
  return gisland::ProcessSpec{
      .argv = {GISLAND_FAKE_MODULE_PATH, std::move(mode)},
      .environment = {},
      .working_directory = std::nullopt,
  };
}

[[nodiscard]] std::size_t open_descriptor_count() {
  return static_cast<std::size_t>(std::distance(
      std::filesystem::directory_iterator{"/proc/self/fd"}, std::filesystem::directory_iterator{}));
}

} // namespace

TEST_CASE("POSIX backend propagates argv environment and cwd without a shell") {
  TemporaryDirectory directory;
  gisland::ProcessBackend backend;
  const auto shell_marker = directory.path() / "shell-was-run";
  auto spec = fake_spec("inspect");
  spec.argv.emplace_back("plain");
  spec.argv.emplace_back("two words");
  spec.argv.emplace_back("$(touch " + shell_marker.string() + ")");
  spec.environment.emplace("GISLAND_FAKE_VALUE", "overridden");
  spec.working_directory = directory.path();

  int stdin_descriptor = -1;
  int stdout_descriptor = -1;
  int stderr_descriptor = -1;
  pid_t child_pid = -1;
  {
    auto spawned = backend.spawn(spec);
    REQUIRE(spawned.has_value());
    auto process = std::move(*spawned);
    child_pid = process.pid();
    stdin_descriptor = process.stdin_fd();
    stdout_descriptor = process.stdout_fd();
    stderr_descriptor = process.stderr_fd();

    CHECK(child_pid > 0);
    CHECK(process.process_group() == child_pid);
    CHECK(::getpgid(child_pid) == child_pid);
    CHECK(::getpgrp() != child_pid);
    for (const int descriptor : {stdin_descriptor, stdout_descriptor, stderr_descriptor}) {
      REQUIRE(::fcntl(descriptor, F_GETFL) >= 0);
      CHECK((::fcntl(descriptor, F_GETFL) & O_NONBLOCK) != 0);
      CHECK((::fcntl(descriptor, F_GETFD) & FD_CLOEXEC) != 0);
    }

    const auto report = nlohmann::json::parse(read_stdout_line(backend, process));
    CHECK(report.at("arguments") ==
          nlohmann::json{"plain", "two words", "$(touch " + shell_marker.string() + ")"});
    CHECK(report.at("environment") == "overridden");
    CHECK(report.at("working_directory") == directory.path().string());
    CHECK_FALSE(report.at("stdin_nonblocking").get<bool>());
    CHECK_FALSE(report.at("stdout_nonblocking").get<bool>());
    CHECK_FALSE(report.at("stderr_nonblocking").get<bool>());
    CHECK_FALSE(std::filesystem::exists(shell_marker));

    const auto exit = wait_for_exit(backend, process);
    CHECK(exit.kind == gisland::ExitKind::exited);
    CHECK(exit.code == 0);
  }

  for (const int descriptor : {stdin_descriptor, stdout_descriptor, stderr_descriptor}) {
    errno = 0;
    CHECK(::fcntl(descriptor, F_GETFD) == -1);
    CHECK(errno == EBADF);
  }
  errno = 0;
  CHECK(::waitpid(child_pid, nullptr, WNOHANG) == -1);
  CHECK(errno == ECHILD);
}

TEST_CASE("POSIX backend reports zero nonzero and signal exits") {
  gisland::ProcessBackend backend;

  SECTION("zero") {
    auto spawned = backend.spawn(fake_spec("exit-zero"));
    REQUIRE(spawned.has_value());
    auto process = std::move(*spawned);
    const auto exit = wait_for_exit(backend, process);
    CHECK(exit.kind == gisland::ExitKind::exited);
    CHECK(exit.code == 0);
    CHECK(exit.success());
  }

  SECTION("nonzero") {
    auto spawned = backend.spawn(fake_spec("exit-nonzero"));
    REQUIRE(spawned.has_value());
    auto process = std::move(*spawned);
    const auto exit = wait_for_exit(backend, process);
    CHECK(exit.kind == gisland::ExitKind::exited);
    CHECK(exit.code == 7);
    CHECK_FALSE(exit.success());
  }

  SECTION("signal") {
    auto spawned = backend.spawn(fake_spec("crash"));
    REQUIRE(spawned.has_value());
    auto process = std::move(*spawned);
    const auto exit = wait_for_exit(backend, process);
    CHECK(exit.kind == gisland::ExitKind::signaled);
    CHECK(exit.code == SIGABRT);
    CHECK_FALSE(exit.success());
  }
}

TEST_CASE("POSIX backend exposes independent nonblocking stdin stdout and stderr") {
  gisland::ProcessBackend backend;
  auto spawned = backend.spawn(fake_spec("stderr"));
  REQUIRE(spawned.has_value());
  auto process = std::move(*spawned);

  write_all(backend, process, "{}");
  write_all(backend, process, "\n");
  CHECK(nlohmann::json::parse(read_stdout_line(backend, process)).at("type") == "ready");
  REQUIRE(backend.close_stdin(process).has_value());
  CHECK(read_stderr_to_eof(backend, process) == "fake diagnostic one\nfake diagnostic two\n");

  const auto exit = wait_for_exit(backend, process);
  CHECK(exit.success());
}

TEST_CASE("process-group signaling terminates descendants and reaps the direct child") {
  gisland::ProcessBackend backend;
  auto spawned = backend.spawn(fake_spec("spawn-descendant"));
  REQUIRE(spawned.has_value());
  auto process = std::move(*spawned);
  const pid_t parent_pid = process.pid();

  const auto report = nlohmann::json::parse(read_stdout_line(backend, process));
  const auto descendant_pid = report.at("descendant_pid").get<pid_t>();
  REQUIRE(descendant_pid > 0);
  CHECK(::getpgid(descendant_pid) == process.process_group());

  REQUIRE(backend.signal_group(process, SIGTERM).has_value());
  const auto exit = wait_for_exit(backend, process);
  CHECK(exit.success());

  errno = 0;
  CHECK(::kill(descendant_pid, 0) == -1);
  CHECK(errno == ESRCH);
  errno = 0;
  CHECK(::waitpid(parent_pid, nullptr, WNOHANG) == -1);
  CHECK(errno == ECHILD);
}

TEST_CASE("repeated spawn and reap leaves no parent descriptor open") {
  gisland::ProcessBackend backend;
  const auto before = open_descriptor_count();

  for (int iteration = 0; iteration < 64; ++iteration) {
    auto spawned = backend.spawn(fake_spec("exit-zero"));
    REQUIRE(spawned.has_value());
    auto process = std::move(*spawned);
    CHECK(wait_for_exit(backend, process).success());
  }

  CHECK(open_descriptor_count() == before);
}
