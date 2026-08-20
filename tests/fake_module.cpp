#include <nlohmann/json.hpp>

#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

volatile sig_atomic_t termination_requested = 0;

extern "C" void request_termination(int /*signal*/) { termination_requested = 1; }

void write_json(const nlohmann::json &object) { std::cout << object.dump() << '\n' << std::flush; }

void write_ready() {
  write_json({
      {"type", "ready"},
      {"protocol_major", 1},
      {"protocol_minor", 0},
  });
}

void read_init() {
  std::string line;
  static_cast<void>(std::getline(std::cin, line));
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

[[nodiscard]] int interactive_data() {
  read_init();
  write_json({
      {"type", "ready"},
      {"protocol_major", 1},
      {"protocol_minor", 8},
      {"capabilities", {"data-snapshots"}},
  });
  write_json({{"type", "data"}, {"value", {{"time", "14:35"}}}});

  std::string line;
  bool replaced = false;
  while (std::getline(std::cin, line)) {
    const auto message = nlohmann::json::parse(line, nullptr, false);
    if (!message.is_object()) {
      continue;
    }
    const auto type = message.value("type", "");
    if (type == "shutdown") {
      return EXIT_SUCCESS;
    }
    if (type != "action") {
      continue;
    }
    const std::string action_id = message.value("action_id", "");
    if (const char *path = std::getenv("GISLAND_ACTION_LOG"); path != nullptr) {
      std::ofstream log{path, std::ios::app};
      log << action_id << '\n';
    }
    if (action_id == "first" && !replaced) {
      write_json({{"type", "data"}, {"value", {{"time", "14:36"}}}});
      replaced = true;
    }
    nlohmann::json result{
        {"type", "action_result"},
        {"action_id", action_id},
        {"accepted", true},
    };
    if (message.contains("invocation_id")) {
      result["invocation_id"] = message.at("invocation_id");
    }
    write_json(result);
  }
  return EXIT_SUCCESS;
}

[[nodiscard]] int dismiss_context() {
  read_init();
  write_ready();
  write_json({{"type", "dismiss"}, {"context_id", "retired"}});

  std::string line;
  while (std::getline(std::cin, line)) {
    const auto message = nlohmann::json::parse(line, nullptr, false);
    if (message.is_object() && message.value("type", "") == "shutdown") {
      return EXIT_SUCCESS;
    }
  }
  return EXIT_SUCCESS;
}

[[nodiscard]] int rich_notification() {
  read_init();
  write_json({
      {"type", "ready"},
      {"protocol_major", 1},
      {"protocol_minor", 3},
      {"capabilities", {"context-images", "rich-content"}},
  });
  write_json({
      {"type", "publish"},
      {"context_id", "notification"},
      {"priority", 20},
      {"resources",
       {{{"id", "app-icon"},
         {"format", "rgba8"},
         {"width", 1},
         {"height", 1},
         {"data", "fFz8/w=="}},
        {{"id", "body-image"},
         {"format", "rgba8"},
         {"width", 1},
         {"height", 1},
         {"data", "QOCg/w=="}}}},
      {"compact",
       {{"type", "row"},
        {"gap", "small"},
        {"children",
         {{{"type", "image"},
           {"resource_id", "app-icon"},
           {"role", "notification-icon"},
           {"accessible_label", "Files"}},
          {{"type", "text"},
           {"value", "Download complete: archive.tar.gz is ready"},
           {"role", "compact-primary"}}}}}},
      {"expanded",
       {{"type", "action_region"},
        {"action_id", "default"},
        {"accessible_label", "Open notification"},
        {"content",
         {{"type", "column"},
          {"alignment", "start"},
          {"gap", "small"},
          {"children",
           {{{"type", "row"},
             {"gap", "small"},
             {"children",
              {{{"type", "image"},
                {"resource_id", "app-icon"},
                {"role", "notification-header-icon"},
                {"accessible_label", "Files"}},
               {{"type", "column"},
                {"alignment", "start"},
                {"gap", "xsmall"},
                {"children",
                 {{{"type", "text"}, {"value", "FILES"}, {"role", "caption"}},
                  {{"type", "text"}, {"value", "Download complete"}, {"role", "body"}}}}},
               {{"type", "spacer"}, {"flexible", true}},
               {{"type", "action_region"},
                {"action_id", "close"},
                {"accessible_label", "Close notification"},
                {"content",
                 {{"type", "icon"},
                  {"name", "close"},
                  {"accessible_label", "Close notification"}}}}}}},
            {{"type", "rich_text"},
             {"role", "notification-body"},
             {"content",
              {{{"type", "text"}, {"value", "The file "}},
               {{"type", "text"}, {"value", "archive.tar.gz"}, {"emphasis", {"bold"}}},
               {{"type", "text"}, {"value", " is available in Downloads.\n"}},
               {{"type", "link"},
                {"value", "Open the folder"},
                {"action_id", "open-folder"},
                {"accessible_label", "Open the download folder"}},
               {{"type", "text"}, {"value", "\n"}},
               {{"type", "inline_image"},
                {"resource_id", "body-image"},
                {"role", "notification-inline-image"},
                {"accessible_label", "Downloaded image preview"}}}}},
            {{"type", "row"},
             {"gap", "small"},
             {"children",
              {{{"type", "button"},
                {"action_id", "open"},
                {"accessible_label", "Open download"},
                {"content", {{"type", "text"}, {"value", "Open"}, {"role", "button"}}}},
               {{"type", "button"},
                {"action_id", "dismiss"},
                {"accessible_label", "Dismiss notification"},
                {"content", {{"type", "text"}, {"value", "Dismiss"}, {"role", "button"}}}}}}}}}}}}},
  });

  std::string line;
  while (std::getline(std::cin, line)) {
    const auto message = nlohmann::json::parse(line, nullptr, false);
    if (message.is_object() && message.value("type", "") == "shutdown") {
      return EXIT_SUCCESS;
    }
  }
  return EXIT_SUCCESS;
}

[[nodiscard]] int refuse_stdin() {
  read_init();
  static_cast<void>(::close(STDIN_FILENO));
  write_ready();
  while (true) {
    ::pause();
  }
}

[[nodiscard]] int spawn_descendant() {
  struct sigaction action{};
  action.sa_handler = request_termination;
  ::sigemptyset(&action.sa_mask);
  if (::sigaction(SIGTERM, &action, nullptr) != 0) {
    return 4;
  }

  std::array<int, 2> ready_pipe{};
  if (::pipe2(ready_pipe.data(), O_CLOEXEC) != 0) {
    return 5;
  }

  const pid_t child = ::fork();
  if (child < 0) {
    static_cast<void>(::close(ready_pipe[0]));
    static_cast<void>(::close(ready_pipe[1]));
    return 6;
  }
  if (child == 0) {
    static_cast<void>(::close(ready_pipe[0]));
    struct sigaction default_action{};
    default_action.sa_handler = SIG_DFL;
    ::sigemptyset(&default_action.sa_mask);
    if (::sigaction(SIGTERM, &default_action, nullptr) != 0) {
      _exit(7);
    }
    const char ready = '1';
    if (::write(ready_pipe[1], &ready, sizeof(ready)) != sizeof(ready)) {
      _exit(8);
    }
    static_cast<void>(::close(ready_pipe[1]));
    while (true) {
      ::pause();
    }
  }

  static_cast<void>(::close(ready_pipe[1]));
  char ready = 0;
  ssize_t ready_count = -1;
  do {
    ready_count = ::read(ready_pipe[0], &ready, sizeof(ready));
  } while (ready_count < 0 && errno == EINTR);
  static_cast<void>(::close(ready_pipe[0]));
  if (ready_count != sizeof(ready)) {
    static_cast<void>(::waitpid(child, nullptr, 0));
    return 9;
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

// The mode dispatcher is intentionally flat so each fake behavior is explicit at the call site.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] int run_mode(int argc, char **argv) {
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
  if (mode == "dismiss") {
    return dismiss_context();
  }
  if (mode == "stderr") {
    return protocol_loop(false, true, false);
  }
  if (mode == "ignore-shutdown") {
    ::signal(SIGTERM, SIG_IGN);
    return protocol_loop(false, false, true);
  }
  if (mode == "exit-zero") {
    return EXIT_SUCCESS;
  }
  if (mode == "exit-nonzero") {
    return 7;
  }
  if (mode == "crash") {
    std::abort();
  }
  if (mode == "silent") {
    return silent();
  }
  if (mode == "ignore-stdin") {
    while (true) {
      ::pause();
    }
  }
  if (mode == "ready-ignore-stdin") {
    read_init();
    write_ready();
    while (true) {
      ::pause();
    }
  }
  if (mode == "refuse-stdin") {
    return refuse_stdin();
  }
  if (mode == "spawn-descendant") {
    return spawn_descendant();
  }
  if (mode == "malformed") {
    read_init();
    std::cout << "{not-json}\n" << std::flush;
    return silent();
  }
  if (mode == "duplicate-ready") {
    read_init();
    for (int count = 0; count < 4; ++count) {
      write_ready();
    }
    return silent();
  }
  if (mode == "business-before-ready") {
    read_init();
    write_json({{"type", "dismiss"}, {"context_id", "foreign"}});
    return silent();
  }
  if (mode == "rolling-violations") {
    read_init();
    write_ready();
    for (int count = 0; count < 10; ++count) {
      std::cout << "{bad-json}\n";
      write_json({{"type", "log"}, {"level", "info"}, {"message", "reset"}});
    }
    std::cout << std::flush;
    return silent();
  }
  if (mode == "publish-crash") {
    read_init();
    write_ready();
    write_json({
        {"type", "publish"},
        {"context_id", "fake"},
        {"priority", 7},
        {"compact", {{"type", "text"}, {"value", "fake"}, {"role", "body"}}},
    });
    std::abort();
  }
  if (mode == "incompatible-ready") {
    read_init();
    write_json({{"type", "ready"}, {"protocol_major", 2}, {"protocol_minor", 0}});
    return silent();
  }
  if (mode == "unsupported-capability") {
    read_init();
    write_json({
        {"type", "ready"},
        {"protocol_major", 1},
        {"protocol_minor", 0},
        {"capabilities", {"not-offered"}},
    });
    return silent();
  }
  if (mode == "data" || mode == "delayed-data" || mode == "data-without-capability" ||
      mode == "data-before-ready") {
    read_init();
    if (mode == "data-before-ready") {
      write_json({{"type", "data"}, {"value", {{"time", "14:35"}}}});
      return silent();
    }
    if (mode == "data" || mode == "delayed-data") {
      write_json({
          {"type", "ready"},
          {"protocol_major", 1},
          {"protocol_minor", 1},
          {"capabilities", {"data-snapshots"}},
      });
    } else {
      write_ready();
    }
    if (mode == "delayed-data") {
      std::this_thread::sleep_for(std::chrono::milliseconds{400});
    }
    write_json({{"type", "data"}, {"value", {{"time", "14:35"}}}});
    std::string line;
    while (std::getline(std::cin, line)) {
      const auto message = nlohmann::json::parse(line, nullptr, false);
      if (message.is_object() && message.value("type", "") == "shutdown") {
        return EXIT_SUCCESS;
      }
    }
    return EXIT_SUCCESS;
  }
  if (mode == "content-transition" || mode == "content-transition-without-capability") {
    read_init();
    nlohmann::json ready{{"type", "ready"}, {"protocol_major", 1}, {"protocol_minor", 9}};
    if (mode == "content-transition") {
      ready["capabilities"] = {"data-snapshots", "content-transitions"};
    } else {
      ready["capabilities"] = {"data-snapshots"};
    }
    write_json(ready);
    write_json({{"type", "data"},
                {"value", {{"time", "14:35"}}},
                {"transitions", {{"expanded", "slide-left"}}}});
    return silent();
  }
  if (mode == "independent" || mode == "independent-high" ||
      mode == "independent-without-capability") {
    read_init();
    nlohmann::json ready{{"type", "ready"}, {"protocol_major", 1}, {"protocol_minor", 4}};
    if (mode == "independent" || mode == "independent-high") {
      ready["capabilities"] = {"independent-views"};
    }
    write_json(ready);
    write_json({
        {"type", "publish"},
        {"context_id", "independent"},
        {"priority", mode == "independent-high" ? 1000 : 20},
        {"views",
         {{"expanded", {{"type", "text"}, {"value", "Expanded owner"}, {"role", "body"}}}}},
        {"presentation", {{"reveal", "expanded"}, {"duration_ms", 1000}}},
    });
    std::string line;
    while (std::getline(std::cin, line)) {
      const auto message = nlohmann::json::parse(line, nullptr, false);
      if (message.is_object() && message.value("type", "") == "shutdown") {
        return EXIT_SUCCESS;
      }
    }
    return EXIT_SUCCESS;
  }
  if (mode == "indicator" || mode == "indicator-without-capability" || mode == "indicator-legacy" ||
      mode == "indicator-expanded-legacy" || mode == "indicator-capability-on-1.5") {
    read_init();
    const bool legacy = mode == "indicator-legacy" || mode == "indicator-expanded-legacy" ||
                        mode == "indicator-capability-on-1.5";
    nlohmann::json ready{
        {"type", "ready"}, {"protocol_major", 1}, {"protocol_minor", legacy ? 5 : 6}};
    if (mode == "indicator" || mode == "indicator-capability-on-1.5") {
      ready["capabilities"] = {"status-indicator"};
    }
    write_json(ready);
    nlohmann::json publish{
        {"type", "publish"},
        {"context_id", "indicator"},
        {"priority", 20},
    };
    const nlohmann::json indicator{
        {"type", "indicator"}, {"state", "success"}, {"accessible_label", "Available"}};
    if (mode == "indicator-expanded-legacy") {
      publish["compact"] = {{"type", "text"}, {"value", "Legacy"}, {"role", "body"}};
      publish["expanded"] = indicator;
    } else {
      publish["compact"] = indicator;
    }
    write_json(publish);
    std::string line;
    while (std::getline(std::cin, line)) {
      const auto message = nlohmann::json::parse(line, nullptr, false);
      if (message.is_object() && message.value("type", "") == "shutdown") {
        return EXIT_SUCCESS;
      }
    }
    return EXIT_SUCCESS;
  }
  if (mode == "indicator-effects" || mode == "indicator-effects-without-capability" ||
      mode == "indicator-effects-legacy" || mode == "indicator-effects-capability-on-1.8") {
    read_init();
    const bool legacy =
        mode == "indicator-effects-legacy" || mode == "indicator-effects-capability-on-1.8";
    nlohmann::json ready{
        {"type", "ready"}, {"protocol_major", 1}, {"protocol_minor", legacy ? 8 : 9}};
    ready["capabilities"] = {"status-indicator"};
    if (mode == "indicator-effects" || mode == "indicator-effects-capability-on-1.8") {
      ready["capabilities"].push_back("indicator-effects");
    }
    write_json(ready);
    write_json({
        {"type", "publish"},
        {"context_id", "indicator-effects"},
        {"priority", 20},
        {"compact",
         {{"type", "indicator"},
          {"state", "success"},
          {"accessible_label", "Running"},
          {"effects", {"glow", "breathe"}}}},
    });
    std::string line;
    while (std::getline(std::cin, line)) {
      const auto message = nlohmann::json::parse(line, nullptr, false);
      if (message.is_object() && message.value("type", "") == "shutdown") {
        return EXIT_SUCCESS;
      }
    }
    return EXIT_SUCCESS;
  }
  if (mode == "audio-hud" || mode == "hud-style-without-capability" ||
      mode == "icon-role-without-capability" || mode == "progress-transition-without-capability" ||
      mode == "hud-capability-on-1.6") {
    read_init();
    const bool legacy = mode == "hud-capability-on-1.6";
    nlohmann::json ready{
        {"type", "ready"}, {"protocol_major", 1}, {"protocol_minor", legacy ? 6 : 7}};
    if (mode == "audio-hud") {
      ready["capabilities"] = {"independent-views", "compact-view-styles", "icon-roles",
                               "progress-transitions"};
    } else if (legacy) {
      ready["capabilities"] = {"independent-views", "compact-view-styles"};
    } else {
      ready["capabilities"] = {"independent-views"};
    }
    write_json(ready);

    nlohmann::json compact;
    nlohmann::json publish{{"type", "publish"}, {"context_id", "audio"}, {"priority", 80}};
    if (mode == "hud-style-without-capability" || mode == "hud-capability-on-1.6") {
      compact = {{"type", "text"}, {"value", "HUD"}, {"role", "body"}};
      publish["presentation"] = {{"compact_style", "hud-meter"}};
    } else if (mode == "icon-role-without-capability") {
      compact = {{"type", "icon"},
                 {"name", "volume-high"},
                 {"accessible_label", "Volume"},
                 {"role", "hud-icon"}};
    } else if (mode == "progress-transition-without-capability") {
      compact = {{"type", "progress"}, {"value", 0.5}, {"transition_from", 0.4}};
    } else {
      compact = {{"type", "row"},
                 {"children",
                  {{{"type", "icon"},
                    {"name", "volume-high"},
                    {"accessible_label", "Volume"},
                    {"role", "hud-icon"}},
                   {{"type", "progress"}, {"value", 0.5}, {"transition_from", 0.4}}}}};
      publish["presentation"] = {{"compact_style", "hud-meter"}};
    }
    publish["views"] = {{"compact", std::move(compact)}};
    write_json(publish);
    return silent();
  }
  if (mode == "image" || mode == "image-without-capability") {
    read_init();
    nlohmann::json ready{
        {"type", "ready"},
        {"protocol_major", 1},
        {"protocol_minor", 2},
    };
    if (mode == "image") {
      ready["capabilities"] = {"context-images"};
    }
    write_json(ready);
    write_json({
        {"type", "publish"},
        {"context_id", "image"},
        {"priority", 8},
        {"resources",
         {{{"id", "icon"},
           {"format", "rgba8"},
           {"width", 1},
           {"height", 1},
           {"data", "/wAA/w=="}}}},
        {"compact",
         {{"type", "row"},
          {"gap", "small"},
          {"children",
           {{{"type", "image"},
             {"resource_id", "icon"},
             {"role", "notification-icon"},
             {"accessible_label", "Application"}},
            {{"type", "text"}, {"value", "Image ready"}, {"role", "compact-primary"}}}}}},
    });
    std::string line;
    while (std::getline(std::cin, line)) {
      const auto message = nlohmann::json::parse(line, nullptr, false);
      if (message.is_object() && message.value("type", "") == "shutdown") {
        return EXIT_SUCCESS;
      }
    }
    return EXIT_SUCCESS;
  }
  if (mode == "interactive-data") {
    return interactive_data();
  }
  if (mode == "rich-notification") {
    return rich_notification();
  }
  if (mode == "final-line") {
    read_init();
    write_ready();
    std::cout << nlohmann::json{
        {"type", "publish"},
        {"context_id", "final"},
        {"priority", 1},
        {"compact", {{"type", "text"}, {"value", "final"}, {"role", "body"}}},
    }.dump() << std::flush;
    return EXIT_SUCCESS;
  }
  if (mode == "stderr-long") {
    constexpr std::size_t stderr_bytes = (std::size_t{64} * 1024U) + 128U;
    read_init();
    std::cerr << std::string(stderr_bytes, 'x') << '\n' << std::flush;
    write_ready();
    std::string line;
    while (std::getline(std::cin, line)) {
      const auto message = nlohmann::json::parse(line, nullptr, false);
      if (message.is_object() && message.value("type", "") == "shutdown") {
        return EXIT_SUCCESS;
      }
    }
    return EXIT_SUCCESS;
  }
  return 2;
}

} // namespace

int main(int argc, char **argv) noexcept {
  try {
    return run_mode(argc, argv);
  } catch (...) {
    return 9;
  }
}
