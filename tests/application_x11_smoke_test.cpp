#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <catch2/catch_test_macros.hpp>

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
      setenv("TZ", "UTC", 1);
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
  TemporaryConfig() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    home_ = std::filesystem::temp_directory_path() / ("gisland-smoke-" + std::to_string(suffix));
    action_log_ = home_ / "actions.log";
    application_log_ = home_ / "application.log";
    std::filesystem::create_directories(home_ / "gisland");
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

} // namespace

TEST_CASE("application opens on click, resizes natively, and closes on Escape") {
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
           attributes.map_state == IsViewable && attributes.width == 220 &&
           attributes.height == 64 && attributes.x == 530 && attributes.y == 8;
  }));
  CHECK(attributes.width == 220);
  CHECK(attributes.height == 64);
  CHECK(attributes.x == 530);
  CHECK(attributes.y == 8);

  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display), attributes.x + 110,
                               attributes.y + 32, CurrentTime) != 0);
  REQUIRE(XTestFakeButtonEvent(display, Button1, True, CurrentTime) != 0);
  XSync(display, False);
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  REQUIRE(XTestFakeButtonEvent(display, Button1, False, CurrentTime) != 0);
  XSync(display, False);

  const bool expanded = wait_until([&] {
    XSync(display, False);
    return XGetWindowAttributes(display, *window, &attributes) != 0 && attributes.width == 360 &&
           attributes.height == 300;
  });
  INFO("final geometry: " << attributes.width << 'x' << attributes.height);
  REQUIRE(expanded);
  CHECK(attributes.x == 460);
  CHECK(attributes.y == 8);
  Window focused = None;
  int revert = 0;
  XGetInputFocus(display, &focused, &revert);
  CHECK(focused == *window);

  std::this_thread::sleep_for(std::chrono::milliseconds{800});
  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display), attributes.x + 48,
                               attributes.y + 150, CurrentTime) != 0);
  REQUIRE(XTestFakeButtonEvent(display, Button1, True, CurrentTime) != 0);
  XSync(display, False);
  REQUIRE(XTestFakeButtonEvent(display, Button1, False, CurrentTime) != 0);
  XSync(display, False);
  REQUIRE(wait_until([&] { return read_text(config.action_log()) == "first\n"; }));
  REQUIRE(wait_until([&] {
    return read_text(config.application_log()).find("[clock] action 'first' accepted") !=
           std::string::npos;
  }));
  std::this_thread::sleep_for(std::chrono::milliseconds{20});

  const KeyCode tab = XKeysymToKeycode(display, XK_Tab);
  const KeyCode enter = XKeysymToKeycode(display, XK_Return);
  const KeyCode shift = XKeysymToKeycode(display, XK_Shift_L);
  const KeyCode space = XKeysymToKeycode(display, XK_space);
  REQUIRE(tab != 0);
  REQUIRE(enter != 0);
  REQUIRE(shift != 0);
  REQUIRE(space != 0);
  REQUIRE(XTestFakeKeyEvent(display, tab, True, CurrentTime) != 0);
  XSync(display, False);
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  REQUIRE(XTestFakeKeyEvent(display, tab, False, CurrentTime) != 0);
  XSync(display, False);
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  REQUIRE(XTestFakeKeyEvent(display, enter, True, CurrentTime) != 0);
  XSync(display, False);
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  REQUIRE(XTestFakeKeyEvent(display, enter, False, CurrentTime) != 0);
  XSync(display, False);
  REQUIRE(wait_until([&] { return read_text(config.action_log()) == "first\nlast\n"; }));

  REQUIRE(XTestFakeKeyEvent(display, shift, True, CurrentTime) != 0);
  XSync(display, False);
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  REQUIRE(XTestFakeKeyEvent(display, tab, True, CurrentTime) != 0);
  XSync(display, False);
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  REQUIRE(XTestFakeKeyEvent(display, tab, False, CurrentTime) != 0);
  XSync(display, False);
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  REQUIRE(XTestFakeKeyEvent(display, shift, False, CurrentTime) != 0);
  XSync(display, False);
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  REQUIRE(XTestFakeKeyEvent(display, space, True, CurrentTime) != 0);
  XSync(display, False);
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  REQUIRE(XTestFakeKeyEvent(display, space, False, CurrentTime) != 0);
  XSync(display, False);
  REQUIRE(wait_until([&] { return read_text(config.action_log()) == "first\nlast\nfirst\n"; }));

  const KeyCode escape = XKeysymToKeycode(display, XK_Escape);
  REQUIRE(escape != 0);
  REQUIRE(XTestFakeKeyEvent(display, escape, True, CurrentTime) != 0);
  XSync(display, False);
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  REQUIRE(XTestFakeKeyEvent(display, escape, False, CurrentTime) != 0);
  XSync(display, False);

  REQUIRE(wait_until([&] {
    XSync(display, False);
    return XGetWindowAttributes(display, *window, &attributes) != 0 && attributes.width == 220 &&
           attributes.height == 64;
  }));
  CHECK(attributes.x == 530);
  CHECK(attributes.y == 8);

  XCloseDisplay(display);
}
