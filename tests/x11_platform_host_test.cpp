#include "gisland/x11_platform_host.hpp"

#include "gisland/island.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <optional>
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
    subject_ = XCreateSimpleWindow(display_, root, 20, 20, 300, 100, 0, 0, 0);
    witness_ = XCreateSimpleWindow(display_, root, 400, 20, 120, 80, 0, 0, 0);
    XMapWindow(display_, subject_);
    XMapWindow(display_, witness_);
    XSync(display_, False);
  }

  X11Windows(const X11Windows &) = delete;
  X11Windows &operator=(const X11Windows &) = delete;

  ~X11Windows() {
    XDestroyWindow(display_, witness_);
    XDestroyWindow(display_, subject_);
    XCloseDisplay(display_);
  }

  [[nodiscard]] Display *display() const { return display_; }
  [[nodiscard]] Window subject() const { return subject_; }
  [[nodiscard]] Window witness() const { return witness_; }

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

[[nodiscard]] std::optional<std::uint32_t> cardinal_property(Display *display, Window window,
                                                             Atom property) {
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long count = 0;
  unsigned long remaining = 0;
  unsigned char *bytes = nullptr;
  const int status = XGetWindowProperty(display, window, property, 0, 1, False, XA_CARDINAL,
                                        &actual_type, &actual_format, &count, &remaining, &bytes);
  std::optional<std::uint32_t> value;
  if (status == Success && actual_type == XA_CARDINAL && actual_format == 32 && count == 1) {
    value = static_cast<std::uint32_t>(*reinterpret_cast<const unsigned long *>(bytes));
  }
  if (bytes != nullptr) {
    XFree(bytes);
  }
  return value;
}

} // namespace

TEST_CASE("X11 host publishes stable overlay properties without accepting focus") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  X11Windows windows;
  Window subject = windows.subject();
  auto host = gisland::create_x11_platform_host(&subject);
  REQUIRE(host.has_value());

  XClassHint class_hint{};
  REQUIRE(XGetClassHint(windows.display(), subject, &class_hint) != 0);
  CHECK(std::string_view{class_hint.res_name} == "gisland");
  CHECK(std::string_view{class_hint.res_class} == "Gisland");
  XFree(class_hint.res_name);
  XFree(class_hint.res_class);

  const Atom state = XInternAtom(windows.display(), "_NET_WM_STATE", False);
  const Atom above = XInternAtom(windows.display(), "_NET_WM_STATE_ABOVE", False);
  const Atom sticky = XInternAtom(windows.display(), "_NET_WM_STATE_STICKY", False);
  CHECK(has_atom(windows.display(), subject, state, above));
  CHECK(has_atom(windows.display(), subject, state, sticky));
  const Atom desktop = XInternAtom(windows.display(), "_NET_WM_DESKTOP", False);
  CHECK(has_property(windows.display(), subject, desktop));
  CHECK(cardinal_property(windows.display(), subject, desktop) == 0xFFFFFFFFU);
  CHECK_FALSE(has_property(windows.display(), subject,
                           XInternAtom(windows.display(), "_NET_WM_STRUT", False)));
  CHECK_FALSE(has_property(windows.display(), subject,
                           XInternAtom(windows.display(), "_NET_WM_STRUT_PARTIAL", False)));

  XWMHints *hints = XGetWMHints(windows.display(), subject);
  REQUIRE(hints != nullptr);
  CHECK((hints->flags & InputHint) != 0L);
  CHECK(hints->input == False);
  XFree(hints);
}

TEST_CASE("X11 host resolves output and offsets its shaped input region") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  X11Windows windows;
  Window subject = windows.subject();
  auto host = gisland::create_x11_platform_host(&subject);
  REQUIRE(host.has_value());

  const auto selected = (*host)->select_output("primary");
  REQUIRE(selected.has_value());
  CHECK(selected->output.width > 0);
  CHECK(selected->output.height > 0);

  REQUIRE((*host)
              ->update_input_region(
                  gisland::InputRegion{gisland::geometry_for(gisland::IslandMode::compact),
                                       gisland::IslandPlacement{40.0F, 10.0F}})
              .has_value());
  XSync(windows.display(), False);
  int rectangle_count = 0;
  int ordering = 0;
  XRectangle *rectangles =
      XShapeGetRectangles(windows.display(), subject, ShapeInput, &rectangle_count, &ordering);
  REQUIRE(rectangles != nullptr);
  CHECK(rectangle_count > 1);
  CHECK(rectangles[rectangle_count / 2].x == 40);
  CHECK(rectangles[rectangle_count / 2].y >= 10);
  XFree(rectangles);
}

TEST_CASE("X11 host rejects a missing native window as a recoverable attachment error") {
  const auto host = gisland::create_x11_platform_host(nullptr);

  REQUIRE_FALSE(host.has_value());
  CHECK(host.error().operation == gisland::PlatformOperation::attach_window);
  CHECK(host.error().severity == gisland::PlatformErrorSeverity::recoverable);
}

TEST_CASE("X11 host reports input-region failure for a destroyed window") {
  if (std::getenv("DISPLAY") == nullptr) {
    SKIP("requires an X11 display");
  }
  Display *display = XOpenDisplay(nullptr);
  REQUIRE(display != nullptr);
  Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 100, 40, 0, 0, 0);
  XSync(display, False);
  auto host = gisland::create_x11_platform_host(&window);
  REQUIRE(host.has_value());
  XDestroyWindow(display, window);
  XSync(display, False);

  const auto updated = (*host)->update_input_region(gisland::InputRegion{
      gisland::geometry_for(gisland::IslandMode::compact), gisland::IslandPlacement{0.0F, 0.0F}});

  REQUIRE_FALSE(updated.has_value());
  CHECK(updated.error().operation == gisland::PlatformOperation::update_input_region);
  CHECK(updated.error().severity == gisland::PlatformErrorSeverity::recoverable);
  XCloseDisplay(display);
}
