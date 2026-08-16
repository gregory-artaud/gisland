#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/shape.h>

#include "gisland/control_client.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

class ChildProcess {
public:
  ChildProcess(const std::filesystem::path &config_home,
               const std::filesystem::path &application_log)
      : pid_(fork()) {
    if (pid_ == 0) {
      setenv("XDG_CONFIG_HOME", config_home.c_str(), 1);
      setenv("XDG_RUNTIME_DIR", config_home.c_str(), 1);
      setenv("XDG_STATE_HOME", config_home.c_str(), 1);
      setenv("TZ", "UTC", 1);
      std::string path = std::filesystem::path{GISLAND_CLOCK_CALENDAR_PATH}.parent_path().string();
      path += ':';
      if (const char *existing_path = std::getenv("PATH"); existing_path != nullptr) {
        path += existing_path;
      }
      setenv("PATH", path.c_str(), 1);
      if (std::freopen(application_log.c_str(), "w", stderr) == nullptr) {
        _exit(126);
      }
      execl(GISLAND_BINARY_PATH, GISLAND_BINARY_PATH, nullptr);
      _exit(127);
    }
    if (pid_ < 0) {
      throw std::runtime_error{"could not fork gisland smoke process"};
    }
  }

  ChildProcess(const ChildProcess &) = delete;
  ChildProcess &operator=(const ChildProcess &) = delete;

  ~ChildProcess() {
    if (pid_ > 0) {
      kill(pid_, SIGTERM);
      static_cast<void>(waitpid(pid_, nullptr, 0));
    }
  }

  [[nodiscard]] pid_t pid() const { return pid_; }

private:
  pid_t pid_;
};

class TemporaryConfig {
public:
  explicit TemporaryConfig(bool write_config = true) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    home_ = std::filesystem::temp_directory_path() / ("gisland-smoke-" + std::to_string(suffix));
    action_log_ = home_ / "actions.log";
    application_log_ = home_ / "application.log";
    std::filesystem::create_directories(home_ / "gisland");
    if (!write_config) {
      return;
    }
    std::ofstream config{home_ / "gisland/config.toml"};
    if (!config) {
      throw std::runtime_error{"could not create application smoke config"};
    }
    config << "monitor = \"primary\"\n"
              "theme = \"default\"\n"
              "default_module = \"clock\"\n"
              "[[modules]]\n"
              "id = \"clock\"\n"
              "command = [\""
           << GISLAND_FAKE_MODULE_PATH
           << "\", \"interactive-data\"]\n"
              "restart = \"never\"\n"
              "[modules.environment]\n"
              "GISLAND_ACTION_LOG = \""
           << action_log_.string()
           << "\"\n"
              "[modules.view.compact]\n"
              "type = \"text\"\n"
              "value = { bind = \"time\" }\n"
              "role = \"body\"\n"
              "[modules.view.expanded]\n"
              "type = \"row\"\n"
              "gap = \"normal\"\n"
              "children = [\n"
              "  { type = \"button\", action_id = \"first\", accessible_label = \"First\", content "
              "= { type = \"text\", value = \"First\", role = \"button\" } },\n"
              "  { type = \"button\", action_id = \"disabled\", enabled = false, accessible_label "
              "= \"Disabled\", content = { type = \"text\", value = \"Disabled\", role = "
              "\"button\" } },\n"
              "  { type = \"button\", action_id = \"last\", accessible_label = \"Last\", content = "
              "{ type = \"text\", value = \"Last\", role = \"button\" } }\n"
              "]\n";
  }

  TemporaryConfig(const TemporaryConfig &) = delete;
  TemporaryConfig &operator=(const TemporaryConfig &) = delete;
  ~TemporaryConfig() { std::filesystem::remove_all(home_); }

  [[nodiscard]] const std::filesystem::path &home() const { return home_; }
  [[nodiscard]] const std::filesystem::path &action_log() const { return action_log_; }
  [[nodiscard]] const std::filesystem::path &application_log() const { return application_log_; }
  [[nodiscard]] std::filesystem::path config_path() const { return home_ / "gisland/config.toml"; }

private:
  std::filesystem::path home_;
  std::filesystem::path action_log_;
  std::filesystem::path application_log_;
};

[[nodiscard]] std::string read_text(const std::filesystem::path &path);
void write_text(const std::filesystem::path &path, std::string_view content);

void configure_audio(TemporaryConfig &config, std::string_view state,
                     bool gate_deferred_close = false) {
  const auto bin = config.home() / "bin";
  const auto local_bin = config.home() / ".local/bin";
  std::filesystem::create_directories(bin);
  std::filesystem::create_directories(local_bin);
  std::filesystem::create_symlink(GISLAND_AUDIO_FAKE_PACTL_PATH, bin / "pactl");
  std::filesystem::create_symlink(gate_deferred_close ? GISLAND_AUDIO_FAKE_GISLANDCTL_PATH
                                                      : GISLANDCTL_BINARY_PATH,
                                  local_bin / "gislandctl");
  const auto lua_host = local_bin / "gisland-lua-host";
  std::filesystem::copy_file(GISLAND_LUA_HOST_PATH, lua_host);
  write_text(config.home() / "audio-state.json", state);
  const std::string path = bin.string() + ":/usr/bin:/bin";
  write_text(config.config_path(),
             std::string{"monitor = \"primary\"\n"
                         "theme = \"default\"\n"
                         "[defaults]\n"
                         "compact = \"clock\"\n"
                         "expanded = \"clock\"\n"
                         "[[modules]]\n"
                         "id = \"clock\"\n"
                         "command = [\""} +
                 GISLAND_FAKE_MODULE_PATH +
                 "\", \"interactive-data\"]\n"
                 "restart = \"never\"\n"
                 "[modules.view.compact]\n"
                 "type = \"text\"\n"
                 "value = { bind = \"time\" }\n"
                 "role = \"body\"\n"
                 "[modules.view.expanded]\n"
                 "type = \"text\"\n"
                 "value = \"Calendar\"\n"
                 "role = \"body\"\n"
                 "[[modules]]\n"
                 "id = \"audio\"\n"
                 "command = [\"" +
                 lua_host.string() + "\", \"" + GISLAND_AUDIO_LUA_PATH +
                 "\"]\n"
                 "protocol_min = \"1.8\"\n"
                 "protocol_max = \"1.8\"\n"
                 "restart = \"never\"\n"
                 "[modules.environment]\n"
                 "HOME = \"" +
                 config.home().string() +
                 "\"\n"
                 "PATH = \"" +
                 path +
                 "\"\n"
                 "GISLAND_AUDIO_FAKE_STATE = \"" +
                 (config.home() / "audio-state.json").string() +
                 "\"\n"
                 "GISLAND_AUDIO_COMMAND_LOG = \"" +
                 (config.home() / "audio-commands.jsonl").string() + "\"\n" +
                 (gate_deferred_close ? "GISLAND_AUDIO_FAKE_GISLANDCTL_STARTED = \"" +
                                            (config.home() / "close-started").string() +
                                            "\"\nGISLAND_AUDIO_FAKE_GISLANDCTL_RELEASE = \"" +
                                            (config.home() / "close-release").string() +
                                            "\"\nGISLAND_AUDIO_REAL_GISLANDCTL = \"" +
                                            GISLANDCTL_BINARY_PATH + "\"\n"
                                      : std::string{}));
}

struct CommandResult {
  int status;
  std::string output;
};

[[nodiscard]] CommandResult run_audio_action(const TemporaryConfig &config,
                                             std::string_view action_id) {
  const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto output_path = config.home() / ("control-" + std::to_string(suffix) + ".log");
  const pid_t pid = fork();
  if (pid == 0) {
    setenv("XDG_RUNTIME_DIR", config.home().c_str(), 1);
    if (std::freopen(output_path.c_str(), "w", stdout) == nullptr ||
        std::freopen(output_path.c_str(), "a", stderr) == nullptr) {
      _exit(126);
    }
    const std::string action{action_id};
    execl(GISLANDCTL_BINARY_PATH, GISLANDCTL_BINARY_PATH, "action", "audio", action.c_str(),
          nullptr);
    _exit(127);
  }
  if (pid < 0) {
    throw std::runtime_error{"could not fork gislandctl audio action"};
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      throw std::runtime_error{"could not wait for gislandctl audio action"};
    }
  }
  return {status, read_text(output_path)};
}

[[nodiscard]] bool module_running(const TemporaryConfig &config, std::string_view id) {
  const auto status = gisland::send_control_command((config.home() / "gisland.sock").string(),
                                                    gisland::StatusControl{});
  if (!status) {
    return false;
  }
  const auto &modules = std::get<gisland::ControlStatus>(status->value()).modules;
  return std::ranges::any_of(modules, [id](const auto &module) {
    return module.id == id && module.state == gisland::ControlModuleState::running;
  });
}

[[nodiscard]] std::string read_text(const std::filesystem::path &path) {
  std::ifstream stream{path};
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void write_text(const std::filesystem::path &path, std::string_view content) {
  std::ofstream stream{path};
  if (!stream) {
    throw std::runtime_error{"could not write application smoke fixture"};
  }
  stream << content;
}

[[nodiscard]] std::vector<std::vector<std::string>>
read_pactl_commands(const std::filesystem::path &path) {
  std::vector<std::vector<std::string>> commands;
  std::istringstream lines{read_text(path)};
  for (std::string line; std::getline(lines, line);) {
    const auto record = nlohmann::json::parse(line);
    if (record.at("program") == "pactl") {
      commands.push_back(record.at("argv").get<std::vector<std::string>>());
    }
  }
  return commands;
}

[[nodiscard]] std::optional<pid_t> first_child_pid(pid_t parent) {
  const auto tasks = std::filesystem::path{"/proc"} / std::to_string(parent) / "task";
  std::error_code error;
  for (std::filesystem::directory_iterator iterator{tasks, error}, end; iterator != end;
       iterator.increment(error)) {
    if (error) {
      return std::nullopt;
    }
    std::ifstream stream{iterator->path() / "children"};
    pid_t child = 0;
    if (stream >> child) {
      return child;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<Window> find_gisland_window(Display *display) {
  Window root = DefaultRootWindow(display);
  Window returned_root = None;
  Window returned_parent = None;
  Window *children = nullptr;
  unsigned int child_count = 0;
  if (XQueryTree(display, root, &returned_root, &returned_parent, &children, &child_count) == 0) {
    return std::nullopt;
  }
  std::optional<Window> result;
  for (unsigned int index = 0; index < child_count; ++index) {
    XClassHint hint{};
    if (XGetClassHint(display, children[index], &hint) != 0) {
      const bool matches = hint.res_name != nullptr && std::string_view{hint.res_name} == "gisland";
      XFree(hint.res_name);
      XFree(hint.res_class);
      if (matches) {
        result = children[index];
        break;
      }
    }
  }
  if (children != nullptr) {
    XFree(children);
  }
  return result;
}

template <typename Predicate> [[nodiscard]] bool wait_until(Predicate predicate) {
  for (int attempt = 0; attempt < 150; ++attempt) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }
  return false;
}

struct ShapeBounds {
  int x;
  int y;
  int width;
  int height;
};

struct WindowPosition {
  int x;
  int y;
};

[[nodiscard]] std::optional<WindowPosition> root_position(Display *display, Window window) {
  Window child = None;
  WindowPosition position{};
  if (XTranslateCoordinates(display, window, DefaultRootWindow(display), 0, 0, &position.x,
                            &position.y, &child) == 0) {
    return std::nullopt;
  }
  return position;
}

[[nodiscard]] std::optional<ShapeBounds> input_shape_bounds(Display *display, Window window) {
  int rectangle_count = 0;
  int ordering = 0;
  XRectangle *rectangles =
      XShapeGetRectangles(display, window, ShapeInput, &rectangle_count, &ordering);
  if (rectangles == nullptr || rectangle_count == 0) {
    if (rectangles != nullptr) {
      XFree(rectangles);
    }
    return std::nullopt;
  }
  int minimum_x = rectangles[0].x;
  int minimum_y = rectangles[0].y;
  int maximum_x = rectangles[0].x + rectangles[0].width;
  int maximum_y = rectangles[0].y + rectangles[0].height;
  for (int index = 1; index < rectangle_count; ++index) {
    minimum_x = std::min(minimum_x, static_cast<int>(rectangles[index].x));
    minimum_y = std::min(minimum_y, static_cast<int>(rectangles[index].y));
    maximum_x =
        std::max(maximum_x, static_cast<int>(rectangles[index].x + rectangles[index].width));
    maximum_y =
        std::max(maximum_y, static_cast<int>(rectangles[index].y + rectangles[index].height));
  }
  XFree(rectangles);
  return ShapeBounds{minimum_x, minimum_y, maximum_x - minimum_x, maximum_y - minimum_y};
}

[[nodiscard]] bool send_notification() {
  const pid_t pid = fork();
  if (pid == 0) {
    execlp("notify-send", "notify-send", "--app-name=Files", "--urgency=critical",
           "--expire-time=0", "Download complete", "The archive is ready", nullptr);
    _exit(127);
  }
  if (pid < 0) {
    return false;
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

[[nodiscard]] bool send_history_notification(std::string_view summary) {
  const pid_t pid = fork();
  if (pid == 0) {
    const std::string owned_summary{summary};
    execlp("notify-send", "notify-send", "--app-name=History", "--urgency=normal",
           "--expire-time=500", owned_summary.c_str(), "Stored notification", nullptr);
    _exit(127);
  }
  if (pid < 0) {
    return false;
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

[[nodiscard]] bool open_notification_history(const std::filesystem::path &runtime_directory) {
  const pid_t pid = fork();
  if (pid == 0) {
    setenv("XDG_RUNTIME_DIR", runtime_directory.c_str(), 1);
    setenv("XDG_STATE_HOME", runtime_directory.c_str(), 1);
    std::string path = std::filesystem::path{GISLAND_CLOCK_CALENDAR_PATH}.parent_path().string();
    path += ':';
    if (const char *existing_path = std::getenv("PATH"); existing_path != nullptr) {
      path += existing_path;
    }
    setenv("PATH", path.c_str(), 1);
    execl(GISLAND_NOTIFICATION_HISTORY_PATH, GISLAND_NOTIFICATION_HISTORY_PATH, nullptr);
    _exit(127);
  }
  if (pid < 0) {
    return false;
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

TEST_CASE("application expands on hover and animates within a fixed native canvas") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  Display *display = XOpenDisplay(nullptr);
  REQUIRE(display != nullptr);
  TemporaryConfig config;
  ChildProcess child{config.home(), config.application_log()};
  constexpr int canvas_x = 406;
  constexpr int canvas_y = -4;
  constexpr int canvas_width = 468;
  constexpr int canvas_height = 380;
  constexpr int surface_x = 18;
  constexpr int surface_y = 12;

  std::optional<Window> window;
  REQUIRE(wait_until([&] {
    XSync(display, False);
    window = find_gisland_window(display);
    return window.has_value();
  }));

  XWindowAttributes attributes{};
  REQUIRE(XGetWindowAttributes(display, *window, &attributes) != 0);
  CHECK(attributes.map_state != IsViewable);

  std::optional<WindowPosition> window_position;
  const bool mapped = wait_until([&] {
    XSync(display, False);
    window_position = root_position(display, *window);
    return XGetWindowAttributes(display, *window, &attributes) != 0 &&
           attributes.map_state == IsViewable && attributes.width == canvas_width &&
           attributes.height == canvas_height && window_position &&
           window_position->x == canvas_x && window_position->y == canvas_y;
  });
  INFO("native window: " << (window_position ? window_position->x : -999) << ','
                         << (window_position ? window_position->y : -999) << ' ' << attributes.width
                         << 'x' << attributes.height << " state=" << attributes.map_state);
  REQUIRE(mapped);
  REQUIRE(window_position.has_value());
  CHECK(attributes.width == canvas_width);
  CHECK(attributes.height == canvas_height);
  CHECK(window_position->x == canvas_x);
  CHECK(window_position->y == canvas_y);
  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display), 20, 400, CurrentTime) != 0);
  XSync(display, False);
  std::optional<ShapeBounds> compact_shape;
  const bool compact = wait_until([&] {
    compact_shape = input_shape_bounds(display, *window);
    return compact_shape && compact_shape->width == 230 && compact_shape->height == 32 &&
           compact_shape->x == surface_x + 101 && compact_shape->y == surface_y;
  });
  if (compact_shape) {
    INFO("initial shape: " << compact_shape->x << ',' << compact_shape->y << ' '
                           << compact_shape->width << 'x' << compact_shape->height);
  } else {
    INFO("initial input shape is empty");
  }
  REQUIRE(compact);

  const auto status = gisland::send_control_command((config.home() / "gisland.sock").string(),
                                                    gisland::StatusControl{});
  REQUIRE(status.has_value());
  const auto &snapshot = std::get<gisland::ControlStatus>(status->value());
  REQUIRE(snapshot.compact.has_value());
  CHECK(snapshot.compact->instance_id == "clock");
  REQUIRE(snapshot.modules.size() == 1);
  CHECK(snapshot.modules[0].state == gisland::ControlModuleState::running);

  std::optional<pid_t> module_pid;
  REQUIRE(wait_until([&] {
    module_pid = first_child_pid(child.pid());
    return module_pid.has_value();
  }));
  const std::string original_config = read_text(config.config_path());
  write_text(config.config_path(), "not valid TOML = [\n");
  const auto rejected = gisland::send_control_command((config.home() / "gisland.sock").string(),
                                                      gisland::ReloadControl{});
  REQUIRE(rejected.has_value());
  REQUIRE(std::holds_alternative<gisland::ControlError>(rejected->value()));
  CHECK(std::get<gisland::ControlError>(rejected->value()).code ==
        gisland::ControlErrorCode::reload_rejected);
  const auto retained = gisland::send_control_command((config.home() / "gisland.sock").string(),
                                                      gisland::StatusControl{});
  REQUIRE(retained.has_value());
  CHECK(std::get<gisland::ControlStatus>(retained->value()).modules.size() == 1);

  std::string view_updated_config = original_config;
  const auto binding = view_updated_config.find("{ bind = \"time\" }");
  REQUIRE(binding != std::string::npos);
  view_updated_config.replace(binding, std::string_view{"{ bind = \"time\" }"}.size(),
                              "\"Reloaded\"");
  write_text(config.config_path(), view_updated_config + "\n[[modules]]\n"
                                                         "id = \"disabled\"\n"
                                                         "command = [\"/bin/true\"]\n"
                                                         "enabled = false\n");
  std::this_thread::sleep_for(std::chrono::milliseconds{500});
  const auto reloaded_status = gisland::send_control_command(
      (config.home() / "gisland.sock").string(), gisland::StatusControl{});
  INFO("reload status: " << (reloaded_status.has_value() ? "ok" : reloaded_status.error().message));
  REQUIRE(reloaded_status.has_value());
  const auto &reloaded_snapshot = std::get<gisland::ControlStatus>(reloaded_status->value());
  REQUIRE(reloaded_snapshot.modules.size() == 2);
  CHECK(reloaded_snapshot.modules[0].id == "clock");
  CHECK(reloaded_snapshot.modules[1] ==
        gisland::ModuleControlStatus{"disabled", gisland::ControlModuleState::disabled, false});
  REQUIRE(first_child_pid(child.pid()) == module_pid);

  REQUIRE(gisland::send_control_command((config.home() / "gisland.sock").string(),
                                        gisland::OpenControl{})
              .has_value());
  REQUIRE(wait_until([&] {
    XSync(display, False);
    const auto shape = input_shape_bounds(display, *window);
    return shape && shape->width == 360 && shape->height == 96 && shape->x == surface_x + 36 &&
           shape->y == surface_y;
  }));
  REQUIRE(gisland::send_control_command((config.home() / "gisland.sock").string(),
                                        gisland::CloseControl{})
              .has_value());
  REQUIRE(wait_until([&] {
    XSync(display, False);
    const auto shape = input_shape_bounds(display, *window);
    return shape && shape->width == 230 && shape->height == 32 && shape->x == surface_x + 101 &&
           shape->y == surface_y;
  }));

  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display),
                               window_position->x + surface_x + 216,
                               window_position->y + surface_y + 16, CurrentTime) != 0);
  XSync(display, False);
  REQUIRE(wait_until([&] {
    XSync(display, False);
    const auto shape = input_shape_bounds(display, *window);
    return shape && shape->width == 360 && shape->height == 96 && shape->x == surface_x + 36 &&
           shape->y == surface_y;
  }));
  REQUIRE(XGetWindowAttributes(display, *window, &attributes) != 0);
  window_position = root_position(display, *window);
  REQUIRE(window_position.has_value());
  CHECK(attributes.width == canvas_width);
  CHECK(attributes.height == canvas_height);
  CHECK(window_position->x == canvas_x);
  std::this_thread::sleep_for(std::chrono::milliseconds{800});

  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display), window_position->x + surface_x + 84,
                               window_position->y + surface_y + 48, CurrentTime) != 0);
  REQUIRE(XTestFakeButtonEvent(display, Button1, True, CurrentTime) != 0);
  XSync(display, False);
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  REQUIRE(XTestFakeButtonEvent(display, Button1, False, CurrentTime) != 0);
  XSync(display, False);
  REQUIRE(wait_until([&] { return read_text(config.action_log()) == "first\n"; }));
  REQUIRE(wait_until([&] {
    return read_text(config.application_log()).find("[clock] action 'first' accepted") !=
           std::string::npos;
  }));
  const auto action =
      gisland::send_control_command((config.home() / "gisland.sock").string(),
                                    gisland::ActionControl{"clock", "cli", std::nullopt});
  REQUIRE(action.has_value());
  CHECK(std::holds_alternative<gisland::EmptyControlResult>(action->value()));
  REQUIRE(wait_until([&] { return read_text(config.action_log()) == "first\ncli\n"; }));
  std::this_thread::sleep_for(std::chrono::milliseconds{20});

  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display), 20, 400, CurrentTime) != 0);
  XSync(display, False);
  REQUIRE(wait_until([&] {
    XSync(display, False);
    const auto shape = input_shape_bounds(display, *window);
    return shape && shape->width == 230 && shape->height == 32 && shape->x == surface_x + 101 &&
           shape->y == surface_y;
  }));
  REQUIRE(XGetWindowAttributes(display, *window, &attributes) != 0);
  window_position = root_position(display, *window);
  REQUIRE(window_position.has_value());
  CHECK(attributes.width == canvas_width);
  CHECK(attributes.height == canvas_height);
  CHECK(window_position->x == canvas_x);
  CHECK(window_position->y == canvas_y);

  XCloseDisplay(display);
}

TEST_CASE("application renders the distributed live clock-calendar module") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  Display *display = XOpenDisplay(nullptr);
  REQUIRE(display != nullptr);
  TemporaryConfig config{false};
  ChildProcess child{config.home(), config.application_log()};

  std::optional<Window> window;
  REQUIRE(wait_until([&] {
    XSync(display, False);
    window = find_gisland_window(display);
    return window.has_value();
  }));

  XWindowAttributes attributes{};
  REQUIRE(wait_until([&] {
    XSync(display, False);
    return XGetWindowAttributes(display, *window, &attributes) != 0 &&
           attributes.map_state == IsViewable;
  }));
  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display),
                               attributes.x + (attributes.width / 2), attributes.y + 16,
                               CurrentTime) != 0);
  XSync(display, False);
  REQUIRE(wait_until([&] {
    XSync(display, False);
    const auto shape = input_shape_bounds(display, *window);
    return shape && shape->height > 96 && shape->height < 300;
  }));
  CHECK(read_text(config.application_log()).find("layout:") == std::string::npos);

  XCloseDisplay(display);
}

TEST_CASE("application renders a dynamic image from an external module") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  Display *display = XOpenDisplay(nullptr);
  REQUIRE(display != nullptr);
  TemporaryConfig config;
  write_text(config.config_path(), "monitor = \"primary\"\n"
                                   "theme = \"default\"\n"
                                   "default_module = \"image\"\n"
                                   "[[modules]]\n"
                                   "id = \"image\"\n"
                                   "command = [\"" GISLAND_FAKE_MODULE_PATH "\", \"image\"]\n"
                                   "restart = \"never\"\n");
  ChildProcess child{config.home(), config.application_log()};

  std::optional<Window> window;
  REQUIRE(wait_until([&] {
    XSync(display, False);
    window = find_gisland_window(display);
    return window.has_value();
  }));

  XWindowAttributes attributes{};
  REQUIRE(wait_until([&] {
    XSync(display, False);
    return XGetWindowAttributes(display, *window, &attributes) != 0 &&
           attributes.map_state == IsViewable;
  }));
  std::string status_diagnostic;
  const bool image_selected = wait_until([&] {
    const auto status = gisland::send_control_command((config.home() / "gisland.sock").string(),
                                                      gisland::StatusControl{});
    if (!status) {
      status_diagnostic = status.error().message;
      return false;
    }
    status_diagnostic = gisland::format_control_output(*status, true);
    return std::get<gisland::ControlStatus>(status->value()).compact &&
           std::get<gisland::ControlStatus>(status->value()).compact->instance_id == "image";
  });
  INFO(status_diagnostic);
  INFO(read_text(config.application_log()));
  REQUIRE(image_selected);
  REQUIRE(wait_until([&] {
    const auto shape = input_shape_bounds(display, *window);
    return shape && shape->width == 230 && shape->height == 32;
  }));
  CHECK(read_text(config.application_log()).find("layout:") == std::string::npos);

  XCloseDisplay(display);
}

TEST_CASE("application renders protocol 1.3 rich notification scenes from an external module") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  Display *display = XOpenDisplay(nullptr);
  REQUIRE(display != nullptr);
  TemporaryConfig config;
  write_text(config.config_path(),
             "monitor = \"primary\"\n"
             "theme = \"default\"\n"
             "default_module = \"notification\"\n"
             "[[modules]]\n"
             "id = \"notification\"\n"
             "command = [\"" GISLAND_FAKE_MODULE_PATH "\", \"rich-notification\"]\n"
             "protocol_min = \"1.3\"\n"
             "protocol_max = \"1.3\"\n"
             "restart = \"never\"\n");
  ChildProcess child{config.home(), config.application_log()};

  std::optional<Window> window;
  REQUIRE(wait_until([&] {
    XSync(display, False);
    window = find_gisland_window(display);
    return window.has_value();
  }));

  XWindowAttributes attributes{};
  REQUIRE(wait_until([&] {
    XSync(display, False);
    return XGetWindowAttributes(display, *window, &attributes) != 0 &&
           attributes.map_state == IsViewable;
  }));
  REQUIRE(wait_until([&] {
    XSync(display, False);
    const auto shape = input_shape_bounds(display, *window);
    return shape && shape->height == 32 && shape->width > 230;
  }));

  REQUIRE(gisland::send_control_command((config.home() / "gisland.sock").string(),
                                        gisland::OpenControl{})
              .has_value());
  REQUIRE(wait_until([&] {
    XSync(display, False);
    const auto shape = input_shape_bounds(display, *window);
    return shape && shape->width >= 360 && shape->height > 96 && shape->height < 300;
  }));
  CHECK(read_text(config.application_log()).find("layout:") == std::string::npos);
  CHECK(read_text(config.application_log()).find("render:") == std::string::npos);

  XCloseDisplay(display);
}

TEST_CASE("application keeps compact and expanded owners independent for protocol 1.4") {
  TemporaryConfig config{false};
  write_text(config.config_path(), std::string{"monitor = \"primary\"\n"
                                               "theme = \"default\"\n"
                                               "[defaults]\n"
                                               "compact = \"clock\"\n"
                                               "expanded = \"clock\"\n"
                                               "[[modules]]\n"
                                               "id = \"clock\"\n"
                                               "command = [\""} +
                                       GISLAND_FAKE_MODULE_PATH +
                                       "\", \"interactive-data\"]\n"
                                       "restart = \"never\"\n"
                                       "[modules.view.compact]\n"
                                       "type = \"text\"\n"
                                       "value = { bind = \"time\" }\n"
                                       "role = \"body\"\n"
                                       "[modules.view.expanded]\n"
                                       "type = \"text\"\n"
                                       "value = { bind = \"time\" }\n"
                                       "role = \"body\"\n"
                                       "[[modules]]\n"
                                       "id = \"details\"\n"
                                       "command = [\"" +
                                       GISLAND_FAKE_MODULE_PATH +
                                       "\", \"independent\"]\n"
                                       "restart = \"never\"\n");

  ChildProcess child{config.home(), config.application_log()};
  const std::string socket = (config.home() / "gisland.sock").string();

  REQUIRE(wait_until([&] {
    const auto response = gisland::send_control_command(socket, gisland::StatusControl{});
    if (!response) {
      return false;
    }
    const auto &status = std::get<gisland::ControlStatus>(response->value());
    return status.mode == gisland::IslandMode::expanded && status.compact && status.expanded &&
           status.compact->instance_id == "clock" && status.expanded->instance_id == "details";
  }));

  REQUIRE(wait_until([&] {
    const auto response = gisland::send_control_command(socket, gisland::StatusControl{});
    if (!response) {
      return false;
    }
    const auto &status = std::get<gisland::ControlStatus>(response->value());
    return status.mode == gisland::IslandMode::compact && status.compact && status.expanded &&
           status.compact->instance_id == "clock" && status.expanded->instance_id == "details";
  }));
  CHECK(read_text(config.application_log()).find("layout:") == std::string::npos);
}

TEST_CASE("application renders a freedesktop notification from the shipped daemon") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  Display *display = XOpenDisplay(nullptr);
  REQUIRE(display != nullptr);
  TemporaryConfig config{false};
  ChildProcess child{config.home(), config.application_log()};

  std::optional<Window> window;
  REQUIRE(wait_until([&] {
    XSync(display, False);
    window = find_gisland_window(display);
    return window.has_value();
  }));
  const auto socket = (config.home() / "gisland.sock").string();
  INFO(read_text(config.application_log()));
  REQUIRE(wait_until([&] {
    const auto status = gisland::send_control_command(socket, gisland::StatusControl{});
    if (!status) {
      return false;
    }
    const auto &modules = std::get<gisland::ControlStatus>(status->value()).modules;
    return std::ranges::any_of(modules, [](const auto &module) {
      return module.id == "notifications" && module.state == gisland::ControlModuleState::running;
    });
  }));
  REQUIRE(wait_until(send_notification));

  REQUIRE(wait_until([&] {
    const auto status = gisland::send_control_command(socket, gisland::StatusControl{});
    if (!status) {
      return false;
    }
    const auto &snapshot = std::get<gisland::ControlStatus>(status->value());
    return snapshot.mode == gisland::IslandMode::expanded && snapshot.compact &&
           snapshot.expanded && snapshot.compact->instance_id == "clock" &&
           snapshot.expanded->instance_id == "notifications";
  }));
  REQUIRE(wait_until([&] {
    XSync(display, False);
    const auto shape = input_shape_bounds(display, *window);
    return shape && shape->width >= 360 && shape->height >= 96 && shape->height < 300;
  }));
  REQUIRE(wait_until([&] {
    const auto status = gisland::send_control_command(socket, gisland::StatusControl{});
    return status &&
           std::get<gisland::ControlStatus>(status->value()).mode == gisland::IslandMode::compact;
  }));

  REQUIRE(gisland::send_control_command(socket, gisland::OpenControl{}).has_value());
  REQUIRE(wait_until([&] {
    XSync(display, False);
    const auto shape = input_shape_bounds(display, *window);
    return shape && shape->width >= 360 && shape->height >= 96 && shape->height < 300;
  }));
  const auto application_log = read_text(config.application_log());
  INFO(application_log);
  CHECK(application_log.find("[notifications] layout:") == std::string::npos);

  XCloseDisplay(display);
}

TEST_CASE("external notification history grows on repeated commands and resets after close") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  Display *display = XOpenDisplay(nullptr);
  REQUIRE(display != nullptr);
  TemporaryConfig config{false};
  ChildProcess child{config.home(), config.application_log()};

  std::optional<Window> window;
  REQUIRE(wait_until([&] {
    XSync(display, False);
    window = find_gisland_window(display);
    return window.has_value();
  }));
  const auto socket = (config.home() / "gisland.sock").string();
  REQUIRE(wait_until([&] {
    const auto status = gisland::send_control_command(socket, gisland::StatusControl{});
    if (!status) {
      return false;
    }
    const auto &modules = std::get<gisland::ControlStatus>(status->value()).modules;
    return std::ranges::any_of(modules, [](const auto &module) {
      return module.id == "notifications" && module.state == gisland::ControlModuleState::running;
    });
  }));

  REQUIRE(send_history_notification("First"));
  REQUIRE(send_history_notification("Second"));
  REQUIRE(send_history_notification("Third"));
  REQUIRE(send_history_notification("Fourth"));
  REQUIRE(send_history_notification("Fifth"));
  REQUIRE(send_history_notification("Sixth"));
  REQUIRE(wait_until([&] {
    const auto status = gisland::send_control_command(socket, gisland::StatusControl{});
    return status &&
           std::get<gisland::ControlStatus>(status->value()).mode == gisland::IslandMode::compact;
  }));

  XWindowAttributes initial_attributes{};
  REQUIRE(XGetWindowAttributes(display, *window, &initial_attributes) != 0);

  std::array<int, 5> heights{};
  for (std::size_t index = 0; index < heights.size(); ++index) {
    REQUIRE(open_notification_history(config.home()));
    REQUIRE(wait_until([&] {
      const auto status = gisland::send_control_command(socket, gisland::StatusControl{});
      if (!status) {
        return false;
      }
      const auto &snapshot = std::get<gisland::ControlStatus>(status->value());
      return snapshot.mode == gisland::IslandMode::expanded && snapshot.expanded &&
             snapshot.expanded->instance_id == "notifications" &&
             snapshot.expanded->context_id == "history";
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds{400});
    XSync(display, False);
    const auto shape = input_shape_bounds(display, *window);
    REQUIRE(shape.has_value());
    XWindowAttributes current_attributes{};
    REQUIRE(XGetWindowAttributes(display, *window, &current_attributes) != 0);
    CHECK(current_attributes.width == initial_attributes.width);
    CHECK(current_attributes.height == initial_attributes.height);
    heights[index] = shape->height;
    if (index > 0) {
      CHECK(heights[index] > heights[index - 1]);
    }
  }

  auto shape = input_shape_bounds(display, *window);
  REQUIRE(shape.has_value());
  auto window_position = root_position(display, *window);
  REQUIRE(window_position.has_value());
  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display),
                               window_position->x + shape->x + shape->width / 2,
                               window_position->y + shape->y + 70, CurrentTime) != 0);
  XSync(display, False);
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  REQUIRE(XTestFakeButtonEvent(display, Button1, True, CurrentTime) != 0);
  XSync(display, False);
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  REQUIRE(XTestFakeButtonEvent(display, Button1, False, CurrentTime) != 0);
  XSync(display, False);
  bool saw_intermediate_mask_height = false;
  int last_mask_height = heights.back();
  const bool mask_settled = wait_until([&] {
    XSync(display, False);
    const auto masked_shape = input_shape_bounds(display, *window);
    if (!masked_shape) {
      return false;
    }
    last_mask_height = masked_shape->height;
    saw_intermediate_mask_height =
        saw_intermediate_mask_height ||
        (masked_shape->height < heights.back() && masked_shape->height > heights[3]);
    return masked_shape->height == heights[3];
  });
  INFO("history heights: " << heights[0] << ", " << heights[1] << ", " << heights[2] << ", "
                           << heights[3] << ", " << heights[4]);
  INFO("last masked height: " << last_mask_height);
  REQUIRE(mask_settled);
  CHECK(saw_intermediate_mask_height);

  REQUIRE(open_notification_history(config.home()));
  std::this_thread::sleep_for(std::chrono::milliseconds{400});
  shape = input_shape_bounds(display, *window);
  REQUIRE(shape.has_value());
  CHECK(shape->height == heights.back());

  window_position = root_position(display, *window);
  REQUIRE(window_position.has_value());
  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display),
                               window_position->x + shape->x + shape->width - 28,
                               window_position->y + shape->y + 28, CurrentTime) != 0);
  XSync(display, False);
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  REQUIRE(XTestFakeButtonEvent(display, Button1, True, CurrentTime) != 0);
  XSync(display, False);
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  REQUIRE(XTestFakeButtonEvent(display, Button1, False, CurrentTime) != 0);
  XSync(display, False);
  REQUIRE(wait_until([&] {
    const auto status = gisland::send_control_command(socket, gisland::StatusControl{});
    if (!status) {
      return false;
    }
    const auto &snapshot = std::get<gisland::ControlStatus>(status->value());
    return snapshot.mode == gisland::IslandMode::compact && snapshot.expanded &&
           snapshot.expanded->context_id != "history";
  }));
  REQUIRE(open_notification_history(config.home()));
  std::this_thread::sleep_for(std::chrono::milliseconds{400});
  REQUIRE(wait_until([&] {
    XSync(display, False);
    const auto reset_shape = input_shape_bounds(display, *window);
    return reset_shape && reset_shape->height == heights[0];
  }));
  const auto history_application_log = read_text(config.application_log());
  INFO(history_application_log);
  CHECK(history_application_log.find("[notifications] layout:") == std::string::npos);

  XCloseDisplay(display);
}

TEST_CASE("real control actions drive the Lua audio module through the running application") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }

  SECTION("successful and rejected actions leave the core responsive") {
    TemporaryConfig config{false};
    configure_audio(
        config,
        R"({"volume_reads":[[20],[25],[25],[30]],"mute_reads":[false,false,false,false],"fail_once":"get-sink-volume @DEFAULT_SINK@","failure_message":"sink disappeared"})");
    ChildProcess child{config.home(), config.application_log()};
    REQUIRE(wait_until([&] { return module_running(config, "audio"); }));

    const auto rejected = run_audio_action(config, "volume-up");
    CHECK(WIFEXITED(rejected.status));
    CHECK(WEXITSTATUS(rejected.status) != 0);
    CHECK(rejected.output.find("sink disappeared") != std::string::npos);
    REQUIRE(module_running(config, "audio"));

    const auto successful = run_audio_action(config, "volume-up");
    CHECK(WIFEXITED(successful.status));
    CHECK(WEXITSTATUS(successful.status) == 0);
    CHECK(successful.output == "ok\n");
    REQUIRE(wait_until([&] {
      const auto status = gisland::send_control_command((config.home() / "gisland.sock").string(),
                                                        gisland::StatusControl{});
      return status && std::get<gisland::ControlStatus>(status->value()).compact &&
             std::get<gisland::ControlStatus>(status->value()).compact->instance_id == "audio";
    }));
  }

  SECTION("concurrent actions are correlated independently") {
    TemporaryConfig config{false};
    configure_audio(config, R"({"volume_reads":[[50],[55]],"mute_reads":[false,false]})");
    ChildProcess child{config.home(), config.application_log()};
    REQUIRE(wait_until([&] { return module_running(config, "audio"); }));

    auto accepted =
        std::async(std::launch::async, [&] { return run_audio_action(config, "volume-up"); });
    auto rejected =
        std::async(std::launch::async, [&] { return run_audio_action(config, "unknown"); });
    const auto accepted_result = accepted.get();
    const auto rejected_result = rejected.get();
    CHECK(WIFEXITED(accepted_result.status));
    CHECK(WEXITSTATUS(accepted_result.status) == 0);
    CHECK(accepted_result.output == "ok\n");
    CHECK(WIFEXITED(rejected_result.status));
    CHECK(WEXITSTATUS(rejected_result.status) != 0);
    CHECK(rejected_result.output.find("unknown action") != std::string::npos);
    const std::vector<std::vector<std::string>> expected_commands{
        {"get-sink-volume", "@DEFAULT_SINK@"},        {"get-sink-mute", "@DEFAULT_SINK@"},
        {"set-sink-volume", "@DEFAULT_SINK@", "55%"}, {"get-sink-volume", "@DEFAULT_SINK@"},
        {"get-sink-mute", "@DEFAULT_SINK@"},
    };
    CHECK(read_pactl_commands(config.home() / "audio-commands.jsonl") == expected_commands);
    REQUIRE(module_running(config, "audio"));
  }

  SECTION("a callback timeout is bounded and its late result is harmless") {
    TemporaryConfig config{false};
    configure_audio(
        config,
        R"({"volume_reads":[[20],[25]],"mute_reads":[false,false],"ignore_sigterm_once":"get-sink-volume @DEFAULT_SINK@"})");
    ChildProcess child{config.home(), config.application_log()};
    REQUIRE(wait_until([&] { return module_running(config, "audio"); }));

    auto action =
        std::async(std::launch::async, [&] { return run_audio_action(config, "volume-up"); });
    REQUIRE(wait_until([&] {
      return read_text(config.home() / "audio-commands.jsonl").find("get-sink-volume") !=
             std::string::npos;
    }));
    REQUIRE(action.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
    REQUIRE(module_running(config, "audio"));
    const auto timed_out = action.get();
    CHECK(WIFEXITED(timed_out.status));
    CHECK(WEXITSTATUS(timed_out.status) != 0);
    CHECK(timed_out.output.find("module action timed out") != std::string::npos);
    REQUIRE(wait_until([&] {
      return read_text(config.application_log()).find("timed out after 2.0 seconds") !=
             std::string::npos;
    }));
    REQUIRE(module_running(config, "audio"));

    const auto recovered = run_audio_action(config, "volume-up");
    CHECK(WIFEXITED(recovered.status));
    CHECK(WEXITSTATUS(recovered.status) == 0);
    REQUIRE(module_running(config, "audio"));
  }
}

TEST_CASE("Lua audio publication precedes deferred close without a compact fallback frame") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  TemporaryConfig config{false};
  configure_audio(config, R"({"volume_reads":[[20],[25]],"mute_reads":[false,false]})", true);
  ChildProcess child{config.home(), config.application_log()};
  const std::string socket = (config.home() / "gisland.sock").string();
  REQUIRE(wait_until([&] { return module_running(config, "audio"); }));
  REQUIRE(gisland::send_control_command(socket, gisland::OpenControl{}).has_value());
  REQUIRE(wait_until([&] {
    const auto status = gisland::send_control_command(socket, gisland::StatusControl{});
    return status &&
           std::get<gisland::ControlStatus>(status->value()).mode == gisland::IslandMode::expanded;
  }));

  auto action =
      std::async(std::launch::async, [&] { return run_audio_action(config, "volume-up"); });
  REQUIRE(wait_until([&] { return std::filesystem::exists(config.home() / "close-started"); }));
  bool publication_observed_while_expanded = false;
  bool closed_with_audio = false;
  std::vector<std::string> compact_owners;
  REQUIRE(wait_until([&] {
    const auto response = gisland::send_control_command(socket, gisland::StatusControl{});
    if (!response) {
      return false;
    }
    const auto &status = std::get<gisland::ControlStatus>(response->value());
    if (status.compact) {
      compact_owners.push_back(status.compact->instance_id);
    }
    if (status.compact && status.compact->instance_id == "audio" &&
        status.mode == gisland::IslandMode::expanded) {
      publication_observed_while_expanded = true;
    }
    return publication_observed_while_expanded;
  }));
  write_text(config.home() / "close-release", "release\n");
  REQUIRE(wait_until([&] {
    const auto response = gisland::send_control_command(socket, gisland::StatusControl{});
    if (!response) {
      return false;
    }
    const auto &status = std::get<gisland::ControlStatus>(response->value());
    if (status.compact) {
      compact_owners.push_back(status.compact->instance_id);
    }
    closed_with_audio = status.compact && status.compact->instance_id == "audio" &&
                        status.mode == gisland::IslandMode::compact;
    return closed_with_audio;
  }));
  const auto result = action.get();
  REQUIRE(WIFEXITED(result.status));
  REQUIRE(WEXITSTATUS(result.status) == 0);
  REQUIRE(publication_observed_while_expanded);
  REQUIRE(closed_with_audio);
  const auto audio = std::ranges::find(compact_owners, "audio");
  REQUIRE(audio != compact_owners.end());
  CHECK(std::ranges::all_of(audio, compact_owners.end(),
                            [](const auto &owner) { return owner == "audio"; }));
}
