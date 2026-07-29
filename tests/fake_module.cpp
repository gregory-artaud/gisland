#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

volatile sig_atomic_t termination_requested = 0;

extern "C" void request_termination(int /*signal*/) { termination_requested = 1; }

void write_json(const nlohmann::json &object) {
  std::cout << object.dump() << '\n' << std::flush;
}

void write_ready() {
  write_json({
      {"type", "ready"},
      {"protocol_major", 1},
      {"protocol_minor", 0},
  });
}

[[nodiscard]] int inspect(int argc, char **argv) {
  std::vector<std::string> arguments;
  for (int index = 2; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  const char *environment = std::getenv("GISLAND_FAKE_VALUE");
  const int stdin_flags = ::fcntl(STDIN_FILENO, F_GETFL);
  const int stdout_flags = ::fcntl(STDOUT_FILENO, F_GETFL);
  const int stderr_flags = ::fcntl(STDERR_FILENO, F_GETFL);
  write_json({
      {"arguments", arguments},
      {"environment", environment == nullptr ? "" : environment},
      {"working_directory", std::filesystem::current_path().string()},
      {"stdin_nonblocking", stdin_flags >= 0 && (stdin_flags & O_NONBLOCK) != 0},
      {"stdout_nonblocking", stdout_flags >= 0 && (stdout_flags & O_NONBLOCK) != 0},
      {"stderr_nonblocking", stderr_flags >= 0 && (stderr_flags & O_NONBLOCK) != 0},
  });
  return EXIT_SUCCESS;
}

[[nodiscard]] int protocol_loop(bool publish, bool write_stderr, bool ignore_shutdown) {
  std::string line;
  if (!std::getline(std::cin, line)) {
    return 3;
  }

  if (write_stderr) {
    std::cerr << "fake diagnostic one\nfake diagnostic two\n" << std::flush;
  }
  write_ready();
  if (publish) {
    write_json({
        {"type", "publish"},
        {"context_id", "fake"},
        {"priority", 7},
        {"compact", {{"type", "text"}, {"value", "fake"}, {"role", "body"}}},
    });
  }

  while (std::getline(std::cin, line)) {
    const auto message = nlohmann::json::parse(line, nullptr, false);
    if (!message.is_object()) {
      continue;
    }
    const auto type = message.value("type", "");
    if (type == "shutdown") {
      if (!ignore_shutdown) {
        return EXIT_SUCCESS;
      }
      continue;
    }
    if (type == "action") {
      write_json({
          {"type", "action_result"},
          {"action_id", message.value("action_id", "")},
          {"accepted", true},
      });
    } else if (type == "visibility") {
      write_json({
          {"type", "log"},
          {"level", "info"},
          {"message", message.value("visibility", "")},
      });
    }
  }

  if (ignore_shutdown) {
    while (true) {
      ::pause();
    }
  }
  return EXIT_SUCCESS;
}

[[nodiscard]] int silent() {
  std::string line;
  while (std::getline(std::cin, line)) {
  }
  return EXIT_SUCCESS;
}

[[nodiscard]] int spawn_descendant() {
  struct sigaction action {};
  action.sa_handler = request_termination;
  ::sigemptyset(&action.sa_mask);
  if (::sigaction(SIGTERM, &action, nullptr) != 0) {
    return 4;
  }

  const pid_t child = ::fork();
  if (child < 0) {
    return 5;
  }
  if (child == 0) {
    struct sigaction default_action {};
    default_action.sa_handler = SIG_DFL;
    ::sigemptyset(&default_action.sa_mask);
    static_cast<void>(::sigaction(SIGTERM, &default_action, nullptr));
    while (true) {
      ::pause();
    }
  }

  write_json({{"descendant_pid", child}});
  while (termination_requested == 0) {
    ::pause();
  }

  int status = 0;
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    return 2;
  }

  const std::string_view mode{argv[1]};
  if (mode == "inspect") {
    return inspect(argc, argv);
  }
  if (mode == "ready" || mode == "graceful-shutdown") {
    return protocol_loop(false, false, false);
  }
  if (mode == "publish") {
    return protocol_loop(true, false, false);
  }
  if (mode == "stderr") {
    return protocol_loop(false, true, false);
  }
  if (mode == "ignore-shutdown") {
    return protocol_loop(false, false, true);
  }
  if (mode == "exit-zero") {
    return EXIT_SUCCESS;
  }
  if (mode == "exit-nonzero") {
    return 7;
  }
  if (mode == "crash") {
    ::raise(SIGSEGV);
    return 8;
  }
  if (mode == "silent") {
    return silent();
  }
  if (mode == "ignore-stdin") {
    while (true) {
      ::pause();
    }
  }
  if (mode == "spawn-descendant") {
    return spawn_descendant();
  }
  if (mode == "malformed") {
    std::string line;
    static_cast<void>(std::getline(std::cin, line));
    std::cout << "{not-json}\n" << std::flush;
    return silent();
  }
  if (mode == "duplicate-ready") {
    std::string line;
    static_cast<void>(std::getline(std::cin, line));
    write_ready();
    write_ready();
    return silent();
  }
  if (mode == "business-before-ready") {
    std::string line;
    static_cast<void>(std::getline(std::cin, line));
    write_json({{"type", "dismiss"}, {"context_id", "foreign"}});
    return silent();
  }
  return 2;
}
