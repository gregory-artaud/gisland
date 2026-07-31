#include "gisland/x11_window_host.hpp"

#include "gisland/island.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/shape.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace {

class X11Windows {
public:
  X11Windows() : display_(XOpenDisplay(nullptr)) {
    if (display_ == nullptr) {
      throw std::runtime_error{"X11 test display is unavailable"};
    }
    const Window root = DefaultRootWindow(display_);
    subject_ = XCreateSimpleWindow(display_, root, 20, 20, 220, 44, 0, 0, 0);
    witness_ = XCreateSimpleWindow(display_, root, 300, 20, 120, 80, 0, 0, 0);
    XSelectInput(display_, witness_, ButtonPressMask | FocusChangeMask);
    XMapWindow(display_, subject_);
    XMapWindow(display_, witness_);
    XSync(display_, False);
  }

  X11Windows(const X11Windows &) = delete;
  X11Windows &operator=(const X11Windows &) = delete;

  ~X11Windows() {
    if (witness_ != None) {
      XDestroyWindow(display_, witness_);
    }
    XDestroyWindow(display_, subject_);
    XCloseDisplay(display_);
  }

  [[nodiscard]] Display *display() const { return display_; }
  [[nodiscard]] Window subject() const { return subject_; }
  [[nodiscard]] Window witness() const { return witness_; }
  void destroy_witness() {
    XDestroyWindow(display_, witness_);
    witness_ = None;
    XSync(display_, False);
  }

private:
  Display *display_;
  Window subject_{};
  Window witness_{};
};

[[nodiscard]] bool has_atom(Display *display, Window window, Atom property, Atom expected) {
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long count = 0;
  unsigned long remaining = 0;
  unsigned char *bytes = nullptr;
  const int status = XGetWindowProperty(display, window, property, 0, 32, False, XA_ATOM,
                                        &actual_type, &actual_format, &count, &remaining, &bytes);
  bool found = false;
  if (status == Success && actual_type == XA_ATOM && actual_format == 32) {
    const auto *atoms = reinterpret_cast<const Atom *>(bytes);
    for (unsigned long index = 0; index < count; ++index) {
      found |= atoms[index] == expected;
    }
  }
  if (bytes != nullptr) {
    XFree(bytes);
  }
  return found;
}

[[nodiscard]] bool has_property(Display *display, Window window, Atom property) {
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long count = 0;
  unsigned long remaining = 0;
  unsigned char *bytes = nullptr;
  const int status = XGetWindowProperty(display, window, property, 0, 1, False, AnyPropertyType,
                                        &actual_type, &actual_format, &count, &remaining, &bytes);
  if (bytes != nullptr) {
    XFree(bytes);
  }
  return status == Success && actual_type != None;
}

[[nodiscard]] bool has_event(const std::vector<gisland::X11WindowEvent> &events,
                             gisland::X11WindowEventKind kind) {
  return std::ranges::any_of(events, [kind](const auto &event) { return event.kind == kind; });
}

} // namespace

TEST_CASE("X11 host publishes stable overlay properties and compact focus policy") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  X11Windows windows;
  Window subject = windows.subject();
  auto host = gisland::X11WindowHost::create(&subject);
  REQUIRE(host.has_value());

  XClassHint class_hint{};
  REQUIRE(XGetClassHint(windows.display(), subject, &class_hint) != 0);
  CHECK(std::string_view{class_hint.res_name} == "gisland");
  CHECK(std::string_view{class_hint.res_class} == "Gisland");
  XFree(class_hint.res_name);
  XFree(class_hint.res_class);

  const Atom state = XInternAtom(windows.display(), "_NET_WM_STATE", False);
  const Atom above = XInternAtom(windows.display(), "_NET_WM_STATE_ABOVE", False);
  CHECK(has_atom(windows.display(), subject, state, above));
  const Atom strut = XInternAtom(windows.display(), "_NET_WM_STRUT", False);
  const Atom partial_strut = XInternAtom(windows.display(), "_NET_WM_STRUT_PARTIAL", False);
  CHECK_FALSE(has_property(windows.display(), subject, strut));
  CHECK_FALSE(has_property(windows.display(), subject, partial_strut));

  XWMHints *hints = XGetWMHints(windows.display(), subject);
  REQUIRE(hints != nullptr);
  CHECK((hints->flags & InputHint) != 0L);
  CHECK(hints->input == False);
  XFree(hints);
}

TEST_CASE("X11 host resolves active RandR output and applies local rounded shape") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  X11Windows windows;
  Window subject = windows.subject();
  auto host = gisland::X11WindowHost::create(&subject);
  REQUIRE(host.has_value());

  const auto selected = host->select_output("primary");
  REQUIRE(selected.has_value());
  CHECK(selected->monitor.width > 0);
  CHECK(selected->monitor.height > 0);

  REQUIRE(host->apply_shape(gisland::geometry_for(gisland::IslandMode::compact)).has_value());
  XSync(windows.display(), False);
  int rectangle_count = 0;
  int ordering = 0;
  XRectangle *rectangles =
      XShapeGetRectangles(windows.display(), subject, ShapeInput, &rectangle_count, &ordering);
  REQUIRE(rectangles != nullptr);
  CHECK(rectangle_count > 1);
  CHECK(rectangles[rectangle_count / 2].x == 0);
  XFree(rectangles);
}

TEST_CASE("X11 host acquires expanded focus and restores the previous viewable window") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  X11Windows windows;
  XSetInputFocus(windows.display(), windows.witness(), RevertToPointerRoot, CurrentTime);
  XSync(windows.display(), False);
  Window subject = windows.subject();
  auto host = gisland::X11WindowHost::create(&subject);
  REQUIRE(host.has_value());

  REQUIRE(host->enter_expanded().has_value());
  XSync(windows.display(), False);
  Window focused = None;
  int revert = 0;
  XGetInputFocus(windows.display(), &focused, &revert);
  CHECK(focused == subject);

  REQUIRE(host->leave_expanded(true).has_value());
  XSync(windows.display(), False);
  XGetInputFocus(windows.display(), &focused, &revert);
  CHECK(focused == windows.witness());
}

TEST_CASE("X11 host reports focus loss without stealing focus back") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  X11Windows windows;
  Window subject = windows.subject();
  auto host = gisland::X11WindowHost::create(&subject);
  REQUIRE(host.has_value());
  REQUIRE(host->enter_expanded().has_value());

  XSetInputFocus(windows.display(), windows.witness(), RevertToPointerRoot, CurrentTime);
  XSync(windows.display(), False);
  const auto events = host->poll_events();
  REQUIRE(events.has_value());
  CHECK(has_event(*events, gisland::X11WindowEventKind::focus_lost));

  REQUIRE(host->leave_expanded(false).has_value());
  Window focused = None;
  int revert = 0;
  XGetInputFocus(windows.display(), &focused, &revert);
  CHECK(focused == windows.witness());
}

TEST_CASE("X11 host reports active grab failure without changing focus policy") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  X11Windows windows;
  XSetInputFocus(windows.display(), windows.witness(), RevertToPointerRoot, CurrentTime);
  const int competing_grab =
      XGrabPointer(windows.display(), windows.witness(), False, ButtonPressMask, GrabModeAsync,
                   GrabModeAsync, None, None, CurrentTime);
  REQUIRE(competing_grab == GrabSuccess);
  XSync(windows.display(), False);
  Window subject = windows.subject();
  auto host = gisland::X11WindowHost::create(&subject);
  REQUIRE(host.has_value());

  const auto entered = host->enter_expanded();
  REQUIRE_FALSE(entered.has_value());
  CHECK(entered.error().code == gisland::X11WindowErrorCode::pointer_grab_failed);
  Window focused = None;
  int revert = 0;
  XGetInputFocus(windows.display(), &focused, &revert);
  CHECK(focused == windows.witness());
  XWMHints *hints = XGetWMHints(windows.display(), subject);
  REQUIRE(hints != nullptr);
  CHECK(hints->input == False);
  XFree(hints);
  XUngrabPointer(windows.display(), CurrentTime);
}

TEST_CASE("X11 host ignores a destroyed previous focus window during restoration") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  X11Windows windows;
  XSetInputFocus(windows.display(), windows.witness(), RevertToPointerRoot, CurrentTime);
  XSync(windows.display(), False);
  Window subject = windows.subject();
  auto host = gisland::X11WindowHost::create(&subject);
  REQUIRE(host.has_value());
  REQUIRE(host->enter_expanded().has_value());

  windows.destroy_witness();
  REQUIRE(host->leave_expanded(true).has_value());
  Window focused = None;
  int revert = 0;
  XGetInputFocus(windows.display(), &focused, &revert);
  CHECK(focused != subject);
}

TEST_CASE("X11 host returns a typed error when the compact button grab conflicts") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  X11Windows windows;
  XGrabButton(windows.display(), Button1, AnyModifier, windows.subject(), False, ButtonPressMask,
              GrabModeSync, GrabModeAsync, None, None);
  XSync(windows.display(), False);
  Window subject = windows.subject();

  const auto host = gisland::X11WindowHost::create(&subject);
  REQUIRE_FALSE(host.has_value());
  CHECK(host.error().code == gisland::X11WindowErrorCode::pointer_grab_failed);
  XUngrabButton(windows.display(), Button1, AnyModifier, windows.subject());
}

TEST_CASE("X11 host classifies presses from confirmed server geometry") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  X11Windows windows;
  Window subject = windows.subject();
  auto host = gisland::X11WindowHost::create(&subject);
  REQUIRE(host.has_value());
  XMoveWindow(windows.display(), subject, 500, 100);
  XSync(windows.display(), False);
  REQUIRE(host->poll_events().has_value());
  REQUIRE(host->enter_expanded().has_value());
  REQUIRE(XTestFakeMotionEvent(windows.display(), DefaultScreen(windows.display()), 510, 110,
                               CurrentTime) != 0);
  REQUIRE(XTestFakeButtonEvent(windows.display(), Button1, True, CurrentTime) != 0);
  XSync(windows.display(), False);

  const auto events = host->poll_events();
  REQUIRE(events.has_value());
  CHECK(has_event(*events, gisland::X11WindowEventKind::inside_press));
  REQUIRE(XTestFakeButtonEvent(windows.display(), Button1, False, CurrentTime) != 0);
  XSync(windows.display(), False);
  REQUIRE(host->leave_expanded(false).has_value());
}

TEST_CASE("X11 host treats transparent rounded corners as outside presses") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  X11Windows windows;
  Window subject = windows.subject();
  auto host = gisland::X11WindowHost::create(&subject);
  REQUIRE(host.has_value());
  REQUIRE(host->apply_shape(gisland::geometry_for(gisland::IslandMode::compact)).has_value());
  REQUIRE(host->enter_expanded().has_value());
  REQUIRE(XTestFakeMotionEvent(windows.display(), DefaultScreen(windows.display()), 20, 20,
                               CurrentTime) != 0);
  REQUIRE(XTestFakeButtonEvent(windows.display(), Button1, True, CurrentTime) != 0);
  XSync(windows.display(), False);

  const auto events = host->poll_events();
  REQUIRE(events.has_value());
  CHECK(has_event(*events, gisland::X11WindowEventKind::outside_press));
  REQUIRE(XTestFakeButtonEvent(windows.display(), Button1, False, CurrentTime) != 0);
  XSync(windows.display(), False);
}

TEST_CASE("X11 host replays an outside press to its target and releases the grab") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  X11Windows windows;
  Window subject = windows.subject();
  auto host = gisland::X11WindowHost::create(&subject);
  REQUIRE(host.has_value());
  REQUIRE(XTestFakeMotionEvent(windows.display(), DefaultScreen(windows.display()), 320, 40,
                               CurrentTime) != 0);
  XSync(windows.display(), False);
  REQUIRE(host->enter_expanded().has_value());
  REQUIRE(XTestFakeButtonEvent(windows.display(), Button1, True, CurrentTime) != 0);
  XSync(windows.display(), False);

  const auto events = host->poll_events();
  REQUIRE(events.has_value());
  CHECK(has_event(*events, gisland::X11WindowEventKind::outside_press));
  REQUIRE(XTestFakeButtonEvent(windows.display(), Button1, False, CurrentTime) != 0);
  XSync(windows.display(), False);

  XEvent replayed{};
  CHECK(XCheckWindowEvent(windows.display(), windows.witness(), ButtonPressMask, &replayed) != 0);
  CHECK(replayed.type == ButtonPress);

  const int grab = XGrabPointer(windows.display(), windows.witness(), False, ButtonPressMask,
                                GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
  CHECK(grab == GrabSuccess);
  if (grab == GrabSuccess) {
    XUngrabPointer(windows.display(), CurrentTime);
  }
}
