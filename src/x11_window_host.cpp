#include "gisland/x11_window_host.hpp"

#include "gisland/x11_shape.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace gisland {
namespace {

[[nodiscard]] X11WindowError error(X11WindowErrorCode code, std::string message) {
  return X11WindowError{code, std::move(message)};
}

std::mutex x_error_handler_mutex;
std::atomic<Display *> trapped_display{};
std::atomic<int> captured_x_error{Success};
std::atomic<XErrorHandler> previous_x_error_handler{};

int capture_x_error(Display *display, XErrorEvent *event) {
  if (display == trapped_display.load()) {
    captured_x_error.store(event->error_code);
    return 0;
  }
  if (const auto previous = previous_x_error_handler.load(); previous != nullptr) {
    return previous(display, event);
  }
  return 0;
}

template <typename Request> [[nodiscard]] bool checked_request(Display *display, Request request) {
  const std::lock_guard lock{x_error_handler_mutex};
  captured_x_error.store(Success);
  trapped_display.store(display);
  previous_x_error_handler.store(XSetErrorHandler(capture_x_error));
  request();
  XSync(display, False);
  trapped_display.store(nullptr);
  XSetErrorHandler(previous_x_error_handler.load());
  previous_x_error_handler.store(nullptr);
  return captured_x_error.load() == Success;
}

void set_focusable(Display *display, Window window, bool focusable) {
  XWMHints *hints = XGetWMHints(display, window);
  if (hints == nullptr) {
    hints = XAllocWMHints();
  }
  if (hints == nullptr) {
    return;
  }
  hints->flags |= InputHint;
  hints->input = focusable ? True : False;
  XSetWMHints(display, window, hints);
  XFree(hints);
}

[[nodiscard]] bool is_viewable(Display *display, Window window) {
  if (window == None || window == PointerRoot || window == DefaultRootWindow(display)) {
    return false;
  }
  XWindowAttributes attributes{};
  int exists = 0;
  const bool request_succeeded = checked_request(
      display, [&] { exists = XGetWindowAttributes(display, window, &attributes); });
  return request_succeeded && exists != 0 && attributes.map_state == IsViewable;
}

[[nodiscard]] bool has_window_manager(Display *display) {
  const Window root = DefaultRootWindow(display);
  const Atom property = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", False);
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long count = 0;
  unsigned long remaining = 0;
  unsigned char *bytes = nullptr;
  const bool succeeded = checked_request(display, [&] {
    XGetWindowProperty(display, root, property, 0, 1, False, XA_WINDOW, &actual_type,
                       &actual_format, &count, &remaining, &bytes);
  });
  if (bytes != nullptr) {
    XFree(bytes);
  }
  return succeeded && actual_type == XA_WINDOW && actual_format == 32 && count == 1;
}

void request_focus(Display *display, Window window, bool window_manager_available, Time timestamp) {
  if (!window_manager_available) {
    XSetInputFocus(display, window, RevertToPointerRoot, timestamp);
    return;
  }
  XEvent event{};
  event.xclient.type = ClientMessage;
  event.xclient.window = window;
  event.xclient.message_type = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
  event.xclient.format = 32;
  event.xclient.data.l[0] = 1;
  event.xclient.data.l[1] = static_cast<long>(timestamp);
  XSendEvent(display, DefaultRootWindow(display), False,
             SubstructureRedirectMask | SubstructureNotifyMask, &event);
}

[[nodiscard]] bool contains(const X11RootBounds &bounds, const IslandGeometry &geometry, int root_x,
                            int root_y) {
  const auto local_x = static_cast<std::int64_t>(root_x) - bounds.x;
  const auto local_y = static_cast<std::int64_t>(root_y) - bounds.y;
  if (local_x < 0 || local_y < 0 || local_x >= bounds.width || local_y >= bounds.height) {
    return false;
  }
  const auto rows = rounded_mask_rows(geometry);
  for (const auto &row : rows) {
    if (row.y == local_y) {
      return local_x >= row.x && local_x < static_cast<std::int64_t>(row.x) + row.width;
    }
  }
  return false;
}

} // namespace

struct X11WindowHost::Impl {
  Display *display{};
  Window window{};
  Window previous_focus{};
  bool pointer_grabbed{};
  int randr_event_base{};
  X11RootBounds root_bounds{};
  IslandGeometry geometry{geometry_for(IslandMode::compact)};
  RoundedWindowShape shape;

  ~Impl() {
    if (display != nullptr) {
      static_cast<void>(checked_request(display, [&] {
        if (pointer_grabbed) {
          XUngrabPointer(display, CurrentTime);
        }
        XUngrabButton(display, Button1, AnyModifier, window);
        XRRSelectInput(display, DefaultRootWindow(display), 0);
      }));
      XCloseDisplay(display);
    }
  }
};

X11WindowHost::X11WindowHost(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
X11WindowHost::X11WindowHost(X11WindowHost &&) noexcept = default;
X11WindowHost &X11WindowHost::operator=(X11WindowHost &&) noexcept = default;
X11WindowHost::~X11WindowHost() = default;

std::expected<X11WindowHost, X11WindowError> X11WindowHost::create(void *native_window_handle) {
  if (native_window_handle == nullptr) {
    return std::unexpected(
        error(X11WindowErrorCode::invalid_window, "raylib did not provide an X11 window handle"));
  }
  auto impl = std::make_unique<Impl>();
  impl->window = *static_cast<const XID *>(native_window_handle);
  impl->display = XOpenDisplay(nullptr);
  if (impl->display == nullptr) {
    return std::unexpected(
        error(X11WindowErrorCode::display_unavailable, "could not open the X11 display"));
  }
  XWindowAttributes attributes{};
  int exists = 0;
  const bool window_request_succeeded = checked_request(impl->display, [&] {
    exists = XGetWindowAttributes(impl->display, impl->window, &attributes);
  });
  if (!window_request_succeeded || exists == 0) {
    return std::unexpected(
        error(X11WindowErrorCode::invalid_window, "native X11 window does not exist"));
  }

  char resource_name[] = "gisland";
  char resource_class[] = "Gisland";
  XClassHint class_hint{resource_name, resource_class};
  int randr_error_base = 0;
  if (XRRQueryExtension(impl->display, &impl->randr_event_base, &randr_error_base) == 0) {
    return std::unexpected(
        error(X11WindowErrorCode::randr_unavailable, "XRandR extension is unavailable"));
  }
  const bool configured = checked_request(impl->display, [&] {
    const Atom state = XInternAtom(impl->display, "_NET_WM_STATE", False);
    const Atom above = XInternAtom(impl->display, "_NET_WM_STATE_ABOVE", False);
    XChangeProperty(impl->display, impl->window, state, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(&above), 1);
    XDeleteProperty(impl->display, impl->window,
                    XInternAtom(impl->display, "_NET_WM_STRUT", False));
    XDeleteProperty(impl->display, impl->window,
                    XInternAtom(impl->display, "_NET_WM_STRUT_PARTIAL", False));
    set_focusable(impl->display, impl->window, false);
    XSelectInput(impl->display, impl->window, FocusChangeMask | StructureNotifyMask);
    XRRSelectInput(impl->display, DefaultRootWindow(impl->display),
                   RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputChangeNotifyMask);
  });
  if (!configured) {
    return std::unexpected(
        error(X11WindowErrorCode::request_failed, "could not configure the X11 overlay window"));
  }
  const bool button_grabbed = checked_request(impl->display, [&] {
    XGrabButton(impl->display, Button1, AnyModifier, impl->window, False, ButtonPressMask,
                GrabModeSync, GrabModeAsync, None, None);
  });
  if (!button_grabbed) {
    return std::unexpected(error(X11WindowErrorCode::pointer_grab_failed,
                                 "could not register the compact X11 button grab"));
  }
  if (!checked_request(impl->display,
                       [&] { XSetClassHint(impl->display, impl->window, &class_hint); })) {
    return std::unexpected(
        error(X11WindowErrorCode::request_failed, "could not publish the X11 overlay identity"));
  }
  Window child = None;
  int root_x = 0;
  int root_y = 0;
  if (!checked_request(impl->display,
                       [&] {
                         exists = XGetWindowAttributes(impl->display, impl->window, &attributes);
                         XTranslateCoordinates(impl->display, impl->window,
                                               DefaultRootWindow(impl->display), 0, 0, &root_x,
                                               &root_y, &child);
                       }) ||
      exists == 0) {
    return std::unexpected(
        error(X11WindowErrorCode::request_failed, "could not query the X11 overlay geometry"));
  }
  impl->root_bounds = X11RootBounds{root_x, root_y, attributes.width, attributes.height};
  while (XPending(impl->display) > 0) {
    XEvent event{};
    XNextEvent(impl->display, &event);
  }
  return X11WindowHost{std::move(impl)};
}

std::expected<MonitorSelection, X11WindowError>
X11WindowHost::select_output(std::string_view requested_name) const {
  const Window root = DefaultRootWindow(impl_->display);
  XRRScreenResources *resources = XRRGetScreenResourcesCurrent(impl_->display, root);
  if (resources == nullptr) {
    return std::unexpected(
        error(X11WindowErrorCode::request_failed, "could not query XRandR resources"));
  }
  const RROutput primary = XRRGetOutputPrimary(impl_->display, root);
  std::vector<X11Monitor> monitors;
  for (int index = 0; index < resources->noutput; ++index) {
    const RROutput output = resources->outputs[index];
    XRROutputInfo *output_info = XRRGetOutputInfo(impl_->display, resources, output);
    if (output_info == nullptr) {
      continue;
    }
    if (output_info->connection == RR_Connected && output_info->crtc != None) {
      XRRCrtcInfo *crtc = XRRGetCrtcInfo(impl_->display, resources, output_info->crtc);
      if (crtc != nullptr) {
        if (crtc->width > 0 && crtc->height > 0) {
          monitors.push_back(X11Monitor{
              std::string{output_info->name, static_cast<std::size_t>(output_info->nameLen)},
              crtc->x, crtc->y, static_cast<int>(crtc->width), static_cast<int>(crtc->height),
              output == primary});
        }
        XRRFreeCrtcInfo(crtc);
      }
    }
    XRRFreeOutputInfo(output_info);
  }
  XRRFreeScreenResources(resources);

  auto selected = select_monitor(monitors, requested_name);
  if (!selected) {
    return std::unexpected(error(X11WindowErrorCode::no_active_outputs, selected.error().message));
  }
  return *selected;
}

std::expected<void, X11WindowError>
X11WindowHost::apply_shape(const IslandGeometry &geometry) const {
  impl_->geometry = geometry;
  impl_->shape.apply(&impl_->window, geometry, IslandPlacement{0.0F, 0.0F});
  return {};
}

std::expected<void, X11WindowError> X11WindowHost::enter_expanded(std::uint64_t timestamp) {
  const auto event_time = static_cast<Time>(timestamp);
  Window focused = None;
  int revert = 0;
  XGetInputFocus(impl_->display, &focused, &revert);
  impl_->previous_focus =
      is_viewable(impl_->display, focused) && focused != impl_->window ? focused : None;
  const int status = XGrabPointer(impl_->display, impl_->window, False, ButtonPressMask,
                                  GrabModeSync, GrabModeAsync, None, None, event_time);
  if (status != GrabSuccess) {
    impl_->previous_focus = None;
    return std::unexpected(
        error(X11WindowErrorCode::pointer_grab_failed, "could not grab the X11 pointer"));
  }
  impl_->pointer_grabbed = true;
  const bool window_manager_available = has_window_manager(impl_->display);
  const bool configured = checked_request(impl_->display, [&] {
    set_focusable(impl_->display, impl_->window, true);
    request_focus(impl_->display, impl_->window, window_manager_available, event_time);
    XAllowEvents(impl_->display, SyncPointer, event_time);
  });
  if (!configured) {
    static_cast<void>(checked_request(impl_->display, [&] {
      XUngrabPointer(impl_->display, CurrentTime);
      set_focusable(impl_->display, impl_->window, false);
    }));
    impl_->pointer_grabbed = false;
    impl_->previous_focus = None;
    return std::unexpected(
        error(X11WindowErrorCode::request_failed, "could not activate the expanded X11 window"));
  }
  return {};
}

std::expected<void, X11WindowError> X11WindowHost::leave_expanded(bool restore_focus) {
  const bool can_restore_focus =
      restore_focus && is_viewable(impl_->display, impl_->previous_focus);
  const bool succeeded = checked_request(impl_->display, [&] {
    if (impl_->pointer_grabbed) {
      XUngrabPointer(impl_->display, CurrentTime);
    }
    set_focusable(impl_->display, impl_->window, false);
    if (can_restore_focus) {
      XSetInputFocus(impl_->display, impl_->previous_focus, RevertToPointerRoot, CurrentTime);
    } else if (restore_focus) {
      XSetInputFocus(impl_->display, PointerRoot, RevertToPointerRoot, CurrentTime);
    }
  });
  impl_->pointer_grabbed = false;
  impl_->previous_focus = None;
  if (!succeeded) {
    return std::unexpected(
        error(X11WindowErrorCode::request_failed, "could not leave the expanded X11 window"));
  }
  return {};
}

std::expected<std::vector<X11WindowEvent>, X11WindowError> X11WindowHost::poll_events() {
  std::vector<X11WindowEvent> events;
  if (!checked_request(impl_->display, [] {})) {
    return std::unexpected(error(X11WindowErrorCode::request_failed, "X11 event polling failed"));
  }
  while (XPending(impl_->display) > 0) {
    XEvent event{};
    XNextEvent(impl_->display, &event);
    if (event.type == ButtonPress && !impl_->pointer_grabbed) {
      const PointerButton button =
          event.xbutton.button == Button1 ? PointerButton::primary : PointerButton::other;
      if (!checked_request(impl_->display,
                           [&] { XUngrabPointer(impl_->display, event.xbutton.time); })) {
        return std::unexpected(
            error(X11WindowErrorCode::request_failed, "could not release the compact X11 grab"));
      }
      events.push_back(
          X11WindowEvent{X11WindowEventKind::inside_press, button, event.xbutton.time});
    } else if (event.type == ButtonPress && impl_->pointer_grabbed) {
      const bool inside =
          contains(impl_->root_bounds, impl_->geometry, event.xbutton.x_root, event.xbutton.y_root);
      const PointerButton button =
          event.xbutton.button == Button1 ? PointerButton::primary : PointerButton::other;
      if (inside) {
        if (!checked_request(impl_->display, [&] {
              XAllowEvents(impl_->display, SyncPointer, event.xbutton.time);
            })) {
          return std::unexpected(
              error(X11WindowErrorCode::request_failed, "could not resume the X11 pointer"));
        }
        events.push_back(
            X11WindowEvent{X11WindowEventKind::inside_press, button, event.xbutton.time});
      } else {
        if (!checked_request(impl_->display, [&] {
              XAllowEvents(impl_->display, ReplayPointer, event.xbutton.time);
            })) {
          return std::unexpected(
              error(X11WindowErrorCode::request_failed, "could not replay the X11 pointer press"));
        }
        impl_->pointer_grabbed = false;
        events.push_back(
            X11WindowEvent{X11WindowEventKind::outside_press, button, event.xbutton.time});
      }
    } else if (event.type == ConfigureNotify) {
      Window child = None;
      int root_x = 0;
      int root_y = 0;
      if (checked_request(impl_->display, [&] {
            XTranslateCoordinates(impl_->display, impl_->window, DefaultRootWindow(impl_->display),
                                  0, 0, &root_x, &root_y, &child);
          })) {
        impl_->root_bounds =
            X11RootBounds{root_x, root_y, event.xconfigure.width, event.xconfigure.height};
      }
    } else if (event.type == FocusOut && event.xfocus.detail != NotifyInferior) {
      events.push_back(X11WindowEvent{X11WindowEventKind::focus_lost});
    } else if (event.type == impl_->randr_event_base + RRScreenChangeNotify ||
               event.type == impl_->randr_event_base + RRNotify) {
      events.push_back(X11WindowEvent{X11WindowEventKind::topology_changed});
    }
  }
  return events;
}

} // namespace gisland
