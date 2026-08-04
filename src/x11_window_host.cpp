#include "gisland/x11_window_host.hpp"

#include "gisland/x11_shape.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

#include <atomic>
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

} // namespace

struct X11WindowHost::Impl {
  Display *display{};
  Window window{};
  int randr_event_base{};
  RoundedWindowShape shape;

  ~Impl() {
    if (display != nullptr) {
      static_cast<void>(checked_request(
          display, [&] { XRRSelectInput(display, DefaultRootWindow(display), 0); }));
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
    const Atom states[]{XInternAtom(impl->display, "_NET_WM_STATE_ABOVE", False),
                        XInternAtom(impl->display, "_NET_WM_STATE_STICKY", False)};
    XChangeProperty(impl->display, impl->window, state, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(states), 2);
    const unsigned long all_desktops = 0xFFFFFFFFUL;
    XChangeProperty(impl->display, impl->window,
                    XInternAtom(impl->display, "_NET_WM_DESKTOP", False), XA_CARDINAL, 32,
                    PropModeReplace, reinterpret_cast<const unsigned char *>(&all_desktops), 1);
    XDeleteProperty(impl->display, impl->window,
                    XInternAtom(impl->display, "_NET_WM_STRUT", False));
    XDeleteProperty(impl->display, impl->window,
                    XInternAtom(impl->display, "_NET_WM_STRUT_PARTIAL", False));
    set_focusable(impl->display, impl->window, false);
    Window focused = None;
    int revert = 0;
    XGetInputFocus(impl->display, &focused, &revert);
    if (focused == impl->window) {
      XSetInputFocus(impl->display, PointerRoot, RevertToPointerRoot, CurrentTime);
    }
    XSelectInput(impl->display, impl->window, StructureNotifyMask);
    XRRSelectInput(impl->display, DefaultRootWindow(impl->display),
                   RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputChangeNotifyMask);
  });
  if (!configured) {
    return std::unexpected(
        error(X11WindowErrorCode::request_failed, "could not configure the X11 overlay window"));
  }
  if (!checked_request(impl->display,
                       [&] { XSetClassHint(impl->display, impl->window, &class_hint); })) {
    return std::unexpected(
        error(X11WindowErrorCode::request_failed, "could not publish the X11 overlay identity"));
  }
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
X11WindowHost::apply_shape(const IslandGeometry &geometry, const IslandPlacement &placement) const {
  impl_->shape.apply(&impl_->window, geometry, placement);
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
    if (event.type == impl_->randr_event_base + RRScreenChangeNotify ||
        event.type == impl_->randr_event_base + RRNotify) {
      events.push_back(X11WindowEvent{X11WindowEventKind::topology_changed});
    }
  }
  return events;
}

} // namespace gisland
