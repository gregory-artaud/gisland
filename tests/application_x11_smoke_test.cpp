#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/shape.h>

#include "gisland/control_client.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

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
  constexpr int canvas_x = 442;
  constexpr int canvas_y = -4;
  constexpr int canvas_width = 396;
  constexpr int canvas_height = 132;
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

  const bool mapped = wait_until([&] {
    XSync(display, False);
    return XGetWindowAttributes(display, *window, &attributes) != 0 &&
           attributes.map_state == IsViewable && attributes.width == canvas_width &&
           attributes.height == canvas_height && attributes.x == canvas_x &&
           attributes.y == canvas_y;
  });
  INFO("native window: " << attributes.x << ',' << attributes.y << ' ' << attributes.width << 'x'
                         << attributes.height << " state=" << attributes.map_state);
  REQUIRE(mapped);
  CHECK(attributes.width == canvas_width);
  CHECK(attributes.height == canvas_height);
  CHECK(attributes.x == canvas_x);
  CHECK(attributes.y == canvas_y);
  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display), 20, 400, CurrentTime) != 0);
  XSync(display, False);
  std::optional<ShapeBounds> compact_shape;
  const bool compact = wait_until([&] {
    compact_shape = input_shape_bounds(display, *window);
    return compact_shape && compact_shape->width == 230 && compact_shape->height == 32 &&
           compact_shape->x == surface_x + 65 && compact_shape->y == surface_y;
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
    return shape && shape->width == 360 && shape->height == 96 && shape->x == surface_x &&
           shape->y == surface_y;
  }));
  REQUIRE(gisland::send_control_command((config.home() / "gisland.sock").string(),
                                        gisland::CloseControl{})
              .has_value());
  REQUIRE(wait_until([&] {
    XSync(display, False);
    const auto shape = input_shape_bounds(display, *window);
    return shape && shape->width == 230 && shape->height == 32 && shape->x == surface_x + 65 &&
           shape->y == surface_y;
  }));

  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display), attributes.x + surface_x + 180,
                               attributes.y + surface_y + 16, CurrentTime) != 0);
  XSync(display, False);
  REQUIRE(wait_until([&] {
    XSync(display, False);
    const auto shape = input_shape_bounds(display, *window);
    return shape && shape->width == 360 && shape->height == 96 && shape->x == surface_x &&
           shape->y == surface_y;
  }));
  REQUIRE(XGetWindowAttributes(display, *window, &attributes) != 0);
  CHECK(attributes.width == canvas_width);
  CHECK(attributes.height == canvas_height);
  CHECK(attributes.x == canvas_x);
  std::this_thread::sleep_for(std::chrono::milliseconds{800});

  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display), attributes.x + surface_x + 48,
                               attributes.y + surface_y + 48, CurrentTime) != 0);
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
  std::this_thread::sleep_for(std::chrono::milliseconds{20});

  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display), 20, 400, CurrentTime) != 0);
  XSync(display, False);
  REQUIRE(wait_until([&] {
    XSync(display, False);
    const auto shape = input_shape_bounds(display, *window);
    return shape && shape->width == 230 && shape->height == 32 && shape->x == surface_x + 65 &&
           shape->y == surface_y;
  }));
  REQUIRE(XGetWindowAttributes(display, *window, &attributes) != 0);
  CHECK(attributes.width == canvas_width);
  CHECK(attributes.height == canvas_height);
  CHECK(attributes.x == canvas_x);
  CHECK(attributes.y == canvas_y);

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
    heights[index] = shape->height;
    if (index > 0) {
      CHECK(heights[index] > heights[index - 1]);
    }
  }

  XWindowAttributes attributes{};
  REQUIRE(XGetWindowAttributes(display, *window, &attributes) != 0);
  auto shape = input_shape_bounds(display, *window);
  REQUIRE(shape.has_value());
  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display),
                               attributes.x + shape->x + shape->width / 2,
                               attributes.y + shape->y + 70, CurrentTime) != 0);
  XSync(display, False);
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  REQUIRE(XTestFakeButtonEvent(display, Button1, True, CurrentTime) != 0);
  XSync(display, False);
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  REQUIRE(XTestFakeButtonEvent(display, Button1, False, CurrentTime) != 0);
  XSync(display, False);
  REQUIRE(wait_until([&] {
    XSync(display, False);
    const auto masked_shape = input_shape_bounds(display, *window);
    return masked_shape && masked_shape->height < heights.back();
  }));

  REQUIRE(open_notification_history(config.home()));
  std::this_thread::sleep_for(std::chrono::milliseconds{400});
  shape = input_shape_bounds(display, *window);
  REQUIRE(shape.has_value());
  CHECK(shape->height == heights.back());

  REQUIRE(XGetWindowAttributes(display, *window, &attributes) != 0);
  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display),
                               attributes.x + shape->x + shape->width - 28,
                               attributes.y + shape->y + 28, CurrentTime) != 0);
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
