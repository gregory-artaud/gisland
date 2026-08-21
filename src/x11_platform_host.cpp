#include "gisland/x11_platform_host.hpp"

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

[[nodiscard]] PlatformError error(PlatformOperation operation, PlatformErrorSeverity severity,
                                  std::string message) {
  return PlatformError{operation, severity, std::move(message)};
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

class X11PlatformHost final : public PlatformHost {
public:
  X11PlatformHost(Display *display, Window window, int randr_event_base)
      : display_(display), window_(window), randr_event_base_(randr_event_base), shape_(display) {}

  ~X11PlatformHost() override {
    if (display_ != nullptr) {
      static_cast<void>(checked_request(
          display_, [&] { XRRSelectInput(display_, DefaultRootWindow(display_), 0); }));
      XCloseDisplay(display_);
    }
  }

  [[nodiscard]] std::expected<OutputSelection, PlatformError>
  select_output(std::string_view requested_name) const override;
  [[nodiscard]] std::expected<void, PlatformError>
  update_input_region(const InputRegion &region) const override;
  [[nodiscard]] std::expected<std::vector<PlatformEvent>, PlatformError> poll_events() override;

private:
  Display *display_{};
  Window window_{};
  int randr_event_base_{};
  RoundedWindowShape shape_;
};

std::expected<PlatformHostPtr, PlatformError> create_x11_platform_host(void *native_window_handle) {
  if (native_window_handle == nullptr) {
    return std::unexpected(error(PlatformOperation::attach_window,
                                 PlatformErrorSeverity::recoverable,
                                 "raylib did not provide an X11 window handle"));
  }
  const Window window = *static_cast<const XID *>(native_window_handle);
  Display *display = XOpenDisplay(nullptr);
  if (display == nullptr) {
    return std::unexpected(error(PlatformOperation::connect_display,
                                 PlatformErrorSeverity::recoverable,
                                 "could not open the X11 display"));
  }
  const auto close_display = [&] { XCloseDisplay(display); };
  XWindowAttributes attributes{};
  int exists = 0;
  const bool window_request_succeeded = checked_request(
      display, [&] { exists = XGetWindowAttributes(display, window, &attributes); });
  if (!window_request_succeeded || exists == 0) {
    close_display();
    return std::unexpected(error(PlatformOperation::attach_window,
                                 PlatformErrorSeverity::recoverable,
                                 "native X11 window does not exist"));
  }

  char resource_name[] = "gisland";
  char resource_class[] = "Gisland";
  XClassHint class_hint{resource_name, resource_class};
  int randr_event_base = 0;
  int randr_error_base = 0;
  if (XRRQueryExtension(display, &randr_event_base, &randr_error_base) == 0) {
    close_display();
    return std::unexpected(error(PlatformOperation::query_outputs,
                                 PlatformErrorSeverity::recoverable,
                                 "XRandR extension is unavailable"));
  }
  const bool configured = checked_request(display, [&] {
    const Atom state = XInternAtom(display, "_NET_WM_STATE", False);
    const Atom states[]{XInternAtom(display, "_NET_WM_STATE_ABOVE", False),
                        XInternAtom(display, "_NET_WM_STATE_STICKY", False)};
    XChangeProperty(display, window, state, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(states), 2);
    const unsigned long all_desktops = 0xFFFFFFFFUL;
    XChangeProperty(display, window, XInternAtom(display, "_NET_WM_DESKTOP", False), XA_CARDINAL,
                    32, PropModeReplace, reinterpret_cast<const unsigned char *>(&all_desktops), 1);
    XDeleteProperty(display, window, XInternAtom(display, "_NET_WM_STRUT", False));
    XDeleteProperty(display, window, XInternAtom(display, "_NET_WM_STRUT_PARTIAL", False));
    set_focusable(display, window, false);
    Window focused = None;
    int revert = 0;
    XGetInputFocus(display, &focused, &revert);
    if (focused == window) {
      XSetInputFocus(display, PointerRoot, RevertToPointerRoot, CurrentTime);
    }
    XSelectInput(display, window, StructureNotifyMask);
    XRRSelectInput(display, DefaultRootWindow(display),
                   RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputChangeNotifyMask);
  });
  if (!configured) {
    close_display();
    return std::unexpected(error(PlatformOperation::attach_window,
                                 PlatformErrorSeverity::recoverable,
                                 "could not configure the X11 overlay window"));
  }
  if (!checked_request(display, [&] { XSetClassHint(display, window, &class_hint); })) {
    close_display();
    return std::unexpected(error(PlatformOperation::attach_window,
                                 PlatformErrorSeverity::recoverable,
                                 "could not publish the X11 overlay identity"));
  }
  while (XPending(display) > 0) {
    XEvent event{};
    XNextEvent(display, &event);
  }
  return PlatformHostPtr{std::make_unique<X11PlatformHost>(display, window, randr_event_base)};
}

std::expected<OutputSelection, PlatformError>
X11PlatformHost::select_output(std::string_view requested_name) const {
  const Window root = DefaultRootWindow(display_);
  XRRScreenResources *resources = nullptr;
  RROutput primary = None;
  const bool resources_succeeded = checked_request(display_, [&] {
    resources = XRRGetScreenResourcesCurrent(display_, root);
    primary = XRRGetOutputPrimary(display_, root);
  });
  if (!resources_succeeded || resources == nullptr) {
    return std::unexpected(error(PlatformOperation::query_outputs,
                                 PlatformErrorSeverity::recoverable,
                                 "could not query XRandR resources"));
  }
  std::vector<DisplayOutput> outputs;
  bool query_succeeded = true;
  for (int index = 0; index < resources->noutput; ++index) {
    const RROutput output = resources->outputs[index];
    XRROutputInfo *output_info = nullptr;
    query_succeeded &= checked_request(
        display_, [&] { output_info = XRRGetOutputInfo(display_, resources, output); });
    if (output_info == nullptr) {
      continue;
    }
    if (output_info->connection == RR_Connected && output_info->crtc != None) {
      XRRCrtcInfo *crtc = nullptr;
      query_succeeded &= checked_request(
          display_, [&] { crtc = XRRGetCrtcInfo(display_, resources, output_info->crtc); });
      if (crtc != nullptr) {
        if (crtc->width > 0 && crtc->height > 0) {
          outputs.push_back(DisplayOutput{
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
  if (!query_succeeded) {
    return std::unexpected(error(PlatformOperation::query_outputs,
                                 PlatformErrorSeverity::recoverable,
                                 "could not query XRandR outputs"));
  }

  auto selected = gisland::select_output(outputs, requested_name);
  if (!selected) {
    return std::unexpected(error(PlatformOperation::query_outputs,
                                 PlatformErrorSeverity::recoverable, selected.error().message));
  }
  return *selected;
}

std::expected<void, PlatformError>
X11PlatformHost::update_input_region(const InputRegion &region) const {
  if (!shape_.available()) {
    return std::unexpected(error(PlatformOperation::update_input_region,
                                 PlatformErrorSeverity::recoverable,
                                 "XShape input regions are unavailable"));
  }
  if (!checked_request(display_,
                       [&] { shape_.apply(&window_, region.geometry, region.placement); })) {
    return std::unexpected(error(PlatformOperation::update_input_region,
                                 PlatformErrorSeverity::recoverable,
                                 "could not update the X11 input region"));
  }
  return {};
}

std::expected<std::vector<PlatformEvent>, PlatformError> X11PlatformHost::poll_events() {
  std::vector<PlatformEvent> events;
  if (!checked_request(display_, [] {})) {
    return std::unexpected(error(PlatformOperation::poll_events, PlatformErrorSeverity::recoverable,
                                 "X11 event polling failed"));
  }
  while (XPending(display_) > 0) {
    XEvent event{};
    XNextEvent(display_, &event);
    if (event.type == randr_event_base_ + RRScreenChangeNotify ||
        event.type == randr_event_base_ + RRNotify) {
      events.push_back(PlatformEvent{PlatformEventKind::output_topology_changed});
    }
  }
  return events;
}

} // namespace gisland
