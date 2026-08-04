#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/shape.h>

#include "gisland/control_client.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

private:
  std::filesystem::path home_;
  std::filesystem::path action_log_;
  std::filesystem::path application_log_;
};

[[nodiscard]] std::string read_text(const std::filesystem::path &path) {
  std::ifstream stream{path};
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
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

} // namespace

TEST_CASE("application expands on hover and animates within a fixed native canvas") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  Display *display = XOpenDisplay(nullptr);
  REQUIRE(display != nullptr);
  TemporaryConfig config;
  ChildProcess child{config.home(), config.application_log()};

  std::optional<Window> window;
  REQUIRE(wait_until([&] {
    XSync(display, False);
    window = find_gisland_window(display);
    return window.has_value();
  }));

  XWindowAttributes attributes{};
  REQUIRE(XGetWindowAttributes(display, *window, &attributes) != 0);
  CHECK(attributes.map_state != IsViewable);

  REQUIRE(wait_until([&] {
    XSync(display, False);
    return XGetWindowAttributes(display, *window, &attributes) != 0 &&
           attributes.map_state == IsViewable && attributes.width == 360 &&
           attributes.height == 300 && attributes.x == 460 && attributes.y == 8;
  }));
  CHECK(attributes.width == 360);
  CHECK(attributes.height == 300);
  CHECK(attributes.x == 460);
  CHECK(attributes.y == 8);
  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display), 20, 400, CurrentTime) != 0);
  XSync(display, False);
  std::optional<ShapeBounds> compact_shape;
  const bool compact = wait_until([&] {
    compact_shape = input_shape_bounds(display, *window);
    return compact_shape && compact_shape->width == 220 && compact_shape->height == 64 &&
           compact_shape->x == 70;
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
  REQUIRE(snapshot.active_context.has_value());
  CHECK(snapshot.active_context->instance_id == "clock");
  REQUIRE(snapshot.modules.size() == 1);
  CHECK(snapshot.modules[0].state == gisland::ControlModuleState::running);

  REQUIRE(gisland::send_control_command((config.home() / "gisland.sock").string(),
                                        gisland::OpenControl{})
              .has_value());
  REQUIRE(wait_until([&] {
    XSync(display, False);
    const auto shape = input_shape_bounds(display, *window);
    return shape && shape->width == 360 && shape->height == 300 && shape->x == 0;
  }));
  REQUIRE(gisland::send_control_command((config.home() / "gisland.sock").string(),
                                        gisland::CloseControl{})
              .has_value());
  REQUIRE(wait_until([&] {
    XSync(display, False);
    const auto shape = input_shape_bounds(display, *window);
    return shape && shape->width == 220 && shape->height == 64 && shape->x == 70;
  }));

  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display), attributes.x + 180,
                               attributes.y + 32, CurrentTime) != 0);
  XSync(display, False);
  REQUIRE(wait_until([&] {
    XSync(display, False);
    const auto shape = input_shape_bounds(display, *window);
    return shape && shape->width == 360 && shape->height == 300 && shape->x == 0;
  }));
  REQUIRE(XGetWindowAttributes(display, *window, &attributes) != 0);
  CHECK(attributes.width == 360);
  CHECK(attributes.height == 300);
  CHECK(attributes.x == 460);
  std::this_thread::sleep_for(std::chrono::milliseconds{800});

  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display), attributes.x + 48,
                               attributes.y + 150, CurrentTime) != 0);
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
    return shape && shape->width == 220 && shape->height == 64 && shape->x == 70;
  }));
  REQUIRE(XGetWindowAttributes(display, *window, &attributes) != 0);
  CHECK(attributes.width == 360);
  CHECK(attributes.height == 300);
  CHECK(attributes.x == 460);
  CHECK(attributes.y == 8);

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
                               attributes.x + (attributes.width / 2), attributes.y + 32,
                               CurrentTime) != 0);
  XSync(display, False);
  REQUIRE(wait_until([&] {
    XSync(display, False);
    const auto shape = input_shape_bounds(display, *window);
    return shape && shape->height >= 300;
  }));
  CHECK(read_text(config.application_log()).find("layout:") == std::string::npos);

  XCloseDisplay(display);
}
