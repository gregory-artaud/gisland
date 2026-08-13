#include "wayland_prototype_runtime.hpp"

#include "layer_shell_protocol.hpp"
#include "rlgl_probe.hpp"

#include <EGL/egl.h>
#include <rlgl.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace gisland::wayland_prototype {
namespace {

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int) { stop_requested = 1; }

struct State {
  Options options;
  wl_display *display{};
  wl_registry *registry{};
  wl_compositor *compositor{};
  wl_seat *seat{};
  wl_pointer *pointer{};
  zwlr_layer_shell_v1 *layer_shell{};
  wl_surface *surface{};
  zwlr_layer_surface_v1 *layer_surface{};
  wl_egl_window *egl_window{};
  EGLDisplay egl_display{EGL_NO_DISPLAY};
  EGLContext egl_context{EGL_NO_CONTEXT};
  EGLSurface egl_surface{EGL_NO_SURFACE};
  int width{};
  int height{};
  bool configured{false};
  bool closed{false};
  bool rlgl_ready{false};
  int frames{0};
  std::vector<std::uint32_t> outputs;
};

void registry_global(void *data, wl_registry *registry, std::uint32_t name, const char *interface,
                     std::uint32_t version) {
  auto &state = *static_cast<State *>(data);
  const std::string_view type{interface};
  if (type == wl_compositor_interface.name) {
    state.compositor = static_cast<wl_compositor *>(
        wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4U)));
  } else if (type == wl_seat_interface.name) {
    state.seat = static_cast<wl_seat *>(
        wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5U)));
  } else if (type == zwlr_layer_shell_v1_interface.name) {
    state.layer_shell = static_cast<zwlr_layer_shell_v1 *>(
        wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1));
  } else if (type == wl_output_interface.name) {
    state.outputs.push_back(name);
  }
}

void registry_remove(void *data, wl_registry *, std::uint32_t name) {
  auto &outputs = static_cast<State *>(data)->outputs;
  std::erase(outputs, name);
}

constexpr wl_registry_listener registry_listener{registry_global, registry_remove};

void layer_configure(void *data, zwlr_layer_surface_v1 *surface, std::uint32_t serial,
                     std::uint32_t width, std::uint32_t height) {
  auto &state = *static_cast<State *>(data);
  zwlr_layer_surface_v1_ack_configure(surface, serial);
  state.width = width == 0 ? state.options.width : static_cast<int>(width);
  state.height = height == 0 ? state.options.height : static_cast<int>(height);
  if (state.egl_window != nullptr) {
    wl_egl_window_resize(state.egl_window, state.width, state.height, 0, 0);
  }
  state.configured = true;
}

void layer_closed(void *data, zwlr_layer_surface_v1 *) {
  static_cast<State *>(data)->closed = true;
}

constexpr zwlr_layer_surface_v1_listener layer_listener{layer_configure, layer_closed};

void pointer_enter(void *, wl_pointer *, std::uint32_t, wl_surface *, wl_fixed_t, wl_fixed_t) {}
void pointer_leave(void *, wl_pointer *, std::uint32_t, wl_surface *) {}
void pointer_motion(void *, wl_pointer *, std::uint32_t, wl_fixed_t, wl_fixed_t) {}
void pointer_button(void *, wl_pointer *, std::uint32_t, std::uint32_t, std::uint32_t,
                    std::uint32_t) {}
void pointer_axis(void *, wl_pointer *, std::uint32_t, std::uint32_t, wl_fixed_t) {}
void pointer_frame(void *, wl_pointer *) {}
void pointer_axis_source(void *, wl_pointer *, std::uint32_t) {}
void pointer_axis_stop(void *, wl_pointer *, std::uint32_t, std::uint32_t) {}
void pointer_axis_discrete(void *, wl_pointer *, std::uint32_t, std::int32_t) {}

constexpr wl_pointer_listener pointer_listener{
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = nullptr,
    .axis_relative_direction = nullptr,
};

void seat_capabilities(void *data, wl_seat *seat, std::uint32_t capabilities) {
  auto &state = *static_cast<State *>(data);
  if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0U && state.pointer == nullptr) {
    state.pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(state.pointer, &pointer_listener, &state);
  } else if ((capabilities & WL_SEAT_CAPABILITY_POINTER) == 0U && state.pointer != nullptr) {
    wl_pointer_destroy(state.pointer);
    state.pointer = nullptr;
  }
}

void seat_name(void *, wl_seat *, const char *) {}

constexpr wl_seat_listener seat_listener{seat_capabilities, seat_name};

void destroy(State &state) {
  if (state.rlgl_ready) {
    rlglClose();
  }
  if (state.egl_display != EGL_NO_DISPLAY) {
    eglMakeCurrent(state.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (state.egl_surface != EGL_NO_SURFACE) {
      eglDestroySurface(state.egl_display, state.egl_surface);
    }
    if (state.egl_context != EGL_NO_CONTEXT) {
      eglDestroyContext(state.egl_display, state.egl_context);
    }
  }
  if (state.egl_window != nullptr) {
    wl_egl_window_destroy(state.egl_window);
  }
  if (state.egl_display != EGL_NO_DISPLAY) {
    eglTerminate(state.egl_display);
  }
  if (state.pointer != nullptr) {
    wl_pointer_destroy(state.pointer);
  }
  if (state.seat != nullptr) {
    wl_seat_destroy(state.seat);
  }
  if (state.layer_surface != nullptr) {
    zwlr_layer_surface_v1_destroy(state.layer_surface);
  }
  if (state.surface != nullptr) {
    wl_surface_destroy(state.surface);
  }
  if (state.layer_shell != nullptr) {
    zwlr_layer_shell_v1_destroy(state.layer_shell);
  }
  if (state.compositor != nullptr) {
    wl_compositor_destroy(state.compositor);
  }
  if (state.registry != nullptr) {
    wl_registry_destroy(state.registry);
  }
  if (state.display != nullptr) {
    wl_display_disconnect(state.display);
  }
}

bool initialize_egl(State &state) {
  state.egl_display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(state.display));
  if (state.egl_display == EGL_NO_DISPLAY ||
      eglInitialize(state.egl_display, nullptr, nullptr) == 0 || eglBindAPI(EGL_OPENGL_API) == 0) {
    return false;
  }
  constexpr EGLint attributes[]{EGL_SURFACE_TYPE,
                                EGL_WINDOW_BIT,
                                EGL_RENDERABLE_TYPE,
                                EGL_OPENGL_BIT,
                                EGL_RED_SIZE,
                                8,
                                EGL_GREEN_SIZE,
                                8,
                                EGL_BLUE_SIZE,
                                8,
                                EGL_ALPHA_SIZE,
                                8,
                                EGL_NONE};
  EGLConfig config{};
  EGLint count = 0;
  if (eglChooseConfig(state.egl_display, attributes, &config, 1, &count) == 0 || count == 0) {
    return false;
  }
  constexpr EGLint context_attributes[]{EGL_CONTEXT_MAJOR_VERSION,
                                        3,
                                        EGL_CONTEXT_MINOR_VERSION,
                                        3,
                                        EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                        EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                        EGL_NONE};
  state.egl_context =
      eglCreateContext(state.egl_display, config, EGL_NO_CONTEXT, context_attributes);
  state.egl_window = wl_egl_window_create(state.surface, state.width, state.height);
  if (state.egl_context == EGL_NO_CONTEXT || state.egl_window == nullptr) {
    return false;
  }
  state.egl_surface = eglCreateWindowSurface(
      state.egl_display, config, reinterpret_cast<EGLNativeWindowType>(state.egl_window), nullptr);
  return state.egl_surface != EGL_NO_SURFACE &&
         eglMakeCurrent(state.egl_display, state.egl_surface, state.egl_surface,
                        state.egl_context) != 0;
}

void update_input_region(State &state) {
  wl_region *region = wl_compositor_create_region(state.compositor);
  wl_region_add(region, 20, 20, std::max(1, state.width - 40), std::max(1, state.height - 40));
  wl_surface_set_input_region(state.surface, region);
  wl_region_destroy(region);
}

int display_failure(State &state, std::string_view operation) {
  const int error = wl_display_get_error(state.display);
  std::cerr << operation << " failed: " << error;
  if (error == EPROTO) {
    const wl_interface *interface = nullptr;
    std::uint32_t object_id = 0;
    const std::uint32_t protocol_error =
        wl_display_get_protocol_error(state.display, &interface, &object_id);
    std::cerr << " protocol=" << protocol_error << " object=" << object_id;
    if (interface != nullptr) {
      std::cerr << " interface=" << interface->name;
    }
  }
  std::cerr << '\n';
  destroy(state);
  return 1;
}

void render(State &state) {
  rlSetFramebufferWidth(state.width);
  rlSetFramebufferHeight(state.height);
  rlViewport(0, 0, state.width, state.height);
  rlClearColor(0, 0, 0, 0);
  rlClearScreenBuffers();
  rlMatrixMode(RL_PROJECTION);
  rlLoadIdentity();
  rlOrtho(0.0, static_cast<double>(state.width), static_cast<double>(state.height), 0.0, -1.0, 1.0);
  rlMatrixMode(RL_MODELVIEW);
  rlLoadIdentity();
  const float left = 20.0F;
  const float top = 20.0F;
  const float right = static_cast<float>(state.width - 20);
  const float bottom = static_cast<float>(state.height - 20);
  rlBegin(RL_QUADS);
  rlColor4ub(10, 12, 18, 245);
  rlVertex2f(left, top);
  rlVertex2f(left, bottom);
  rlColor4ub(40, 100, 255, 245);
  rlVertex2f(right, bottom);
  rlVertex2f(right, top);
  rlEnd();
  rlDrawRenderBatchActive();
}

} // namespace

int run(const Options &options) {
  State state{.options = options, .width = options.width, .height = options.height, .outputs = {}};
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::cerr << "Wayland display is unavailable\n";
    return 1;
  }
  state.registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(state.registry, &registry_listener, &state);
  if (wl_display_roundtrip(state.display) < 0 || state.compositor == nullptr ||
      state.layer_shell == nullptr) {
    std::cerr << "Wayland compositor does not advertise wlr-layer-shell\n";
    destroy(state);
    return 1;
  }
  if (state.seat != nullptr) {
    wl_seat_add_listener(state.seat, &seat_listener, &state);
  }
  state.surface = wl_compositor_create_surface(state.compositor);
  state.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
      state.layer_shell, state.surface, nullptr, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      "gisland-wayland-prototype");
  zwlr_layer_surface_v1_add_listener(state.layer_surface, &layer_listener, &state);
  zwlr_layer_surface_v1_set_size(state.layer_surface, static_cast<std::uint32_t>(options.width),
                                 static_cast<std::uint32_t>(options.height));
  zwlr_layer_surface_v1_set_anchor(state.layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP);
  zwlr_layer_surface_v1_set_margin(state.layer_surface, options.top_margin, 0, 0, 0);
  zwlr_layer_surface_v1_set_exclusive_zone(state.layer_surface, 0);
  zwlr_layer_surface_v1_set_keyboard_interactivity(state.layer_surface, 0);
  update_input_region(state);
  wl_surface_commit(state.surface);
  while (!state.configured && !state.closed && wl_display_dispatch(state.display) >= 0) {
  }
  if (!state.configured || !initialize_egl(state)) {
    std::cerr << "Wayland EGL initialization failed: 0x" << std::hex << eglGetError() << '\n';
    destroy(state);
    return 1;
  }
  rlLoadExtensions(reinterpret_cast<void *>(eglGetProcAddress));
  rlglInit(state.width, state.height);
  state.rlgl_ready = true;
  if (rlGetVersion() != RL_OPENGL_33 || rlGetFramebufferWidth() != state.width ||
      rlGetFramebufferHeight() != state.height || gisland_rlgl_has_error()) {
    std::cerr << "rlgl initialization failed\n";
    destroy(state);
    return 1;
  }
  const auto previous_sigint = std::signal(SIGINT, request_stop);
  const auto previous_sigterm = std::signal(SIGTERM, request_stop);
  while (!state.closed && stop_requested == 0) {
    update_input_region(state);
    render(state);
    if (gisland_rlgl_has_error() || eglSwapBuffers(state.egl_display, state.egl_surface) == 0) {
      std::cerr << "Wayland EGL frame presentation failed\n";
      destroy(state);
      return 1;
    }
    ++state.frames;
    if (options.automated && state.frames >= 3) {
      break;
    }
    if (wl_display_roundtrip(state.display) < 0 || wl_display_flush(state.display) < 0) {
      std::signal(SIGINT, previous_sigint);
      std::signal(SIGTERM, previous_sigterm);
      return display_failure(state, "Wayland dispatch");
    }
  }
  std::signal(SIGINT, previous_sigint);
  std::signal(SIGTERM, previous_sigterm);
  destroy(state);
  return 0;
}

} // namespace gisland::wayland_prototype
