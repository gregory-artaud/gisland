#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

class ChildProcess {
public:
  ChildProcess() : pid_(fork()) {
    if (pid_ == 0) {
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
  ChildProcess child;

  std::optional<Window> window;
  REQUIRE(wait_until([&] {
    XSync(display, False);
    window = find_gisland_window(display);
    return window.has_value();
  }));

  XWindowAttributes attributes{};
  REQUIRE(wait_until([&] {
    XSync(display, False);
    return XGetWindowAttributes(display, *window, &attributes) != 0 && attributes.width == 220 &&
           attributes.height == 44 && attributes.x == 530 && attributes.y == 8;
  }));
  CHECK(attributes.width == 220);
  CHECK(attributes.height == 44);
  CHECK(attributes.x == 530);
  CHECK(attributes.y == 8);

  REQUIRE(XTestFakeMotionEvent(display, DefaultScreen(display), attributes.x + 110,
                               attributes.y + 22, CurrentTime) != 0);
  REQUIRE(XTestFakeButtonEvent(display, Button1, True, CurrentTime) != 0);
  XSync(display, False);
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  REQUIRE(XTestFakeButtonEvent(display, Button1, False, CurrentTime) != 0);
  XSync(display, False);

  REQUIRE(wait_until([&] {
    XSync(display, False);
    return XGetWindowAttributes(display, *window, &attributes) != 0 && attributes.width == 420 &&
           attributes.height == 220;
  }));
  CHECK(attributes.x == 430);
  CHECK(attributes.y == 8);
  Window focused = None;
  int revert = 0;
  XGetInputFocus(display, &focused, &revert);
  CHECK(focused == *window);

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
           attributes.height == 44;
  }));
  CHECK(attributes.x == 530);
  CHECK(attributes.y == 8);

  XCloseDisplay(display);
}
