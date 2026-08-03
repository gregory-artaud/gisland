#include "gisland/application.hpp"
#include "gisland/island.hpp"
#include "gisland/layout.hpp"
#include "gisland/module_supervisor.hpp"
#include "gisland/raylib_renderer.hpp"
#include "gisland/runtime.hpp"
#include "gisland/x11_monitor.hpp"
#include "gisland/x11_window_host.hpp"

#include <raylib.h>
#include <rlgl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace gisland {
namespace {

constexpr const char *content_blur_shader = R"glsl(
#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec2 textureSize;
uniform float blurRadius;

out vec4 finalColor;

void main() {
  const float weights[5] = float[5](0.06136, 0.24477, 0.38774, 0.24477, 0.06136);
  vec2 stepSize = (vec2(blurRadius) * 0.5) / textureSize;
  vec4 color = vec4(0.0);
  for (int y = -2; y <= 2; ++y) {
    for (int x = -2; x <= 2; ++x) {
      vec2 offset = vec2(float(x), float(y)) * stepSize;
      color += texture(texture0, fragTexCoord + offset) * weights[x + 2] * weights[y + 2];
    }
  }
  float opacity = colDiffuse.a * fragColor.a;
  finalColor = color * vec4(colDiffuse.rgb * fragColor.rgb * opacity, opacity);
}
)glsl";

class Window final {
public:
  Window(const ApplicationConfig &config, const IslandGeometry &compact) {
    SetConfigFlags(FLAG_WINDOW_TRANSPARENT | FLAG_WINDOW_UNDECORATED | FLAG_WINDOW_TOPMOST |
                   FLAG_WINDOW_ALWAYS_RUN | FLAG_WINDOW_HIDDEN);
    InitWindow(static_cast<int>(std::lround(compact.width)),
               static_cast<int>(std::lround(compact.height)), config.title.c_str());
    SetExitKey(KEY_NULL);
  }

  Window(const Window &) = delete;
  Window &operator=(const Window &) = delete;
  Window(Window &&) = delete;
  Window &operator=(Window &&) = delete;

  ~Window() {
    if (IsWindowReady()) {
      CloseWindow();
    }
  }

  [[nodiscard]] static bool is_ready() { return IsWindowReady(); }

  static void show() { ClearWindowState(FLAG_WINDOW_HIDDEN); }
  static void hide() { SetWindowState(FLAG_WINDOW_HIDDEN); }
};

[[nodiscard]] X11Monitor raylib_monitor_fallback() {
  const int monitor = GetCurrentMonitor();
  const Vector2 origin = GetMonitorPosition(monitor);
  return X11Monitor{GetMonitorName(monitor),
                    static_cast<int>(std::lround(origin.x)),
                    static_cast<int>(std::lround(origin.y)),
                    GetMonitorWidth(monitor),
                    GetMonitorHeight(monitor),
                    true};
}

void draw_content(const RenderTexture2D &texture, const ContentVisual &visual,
                  const IslandGeometry &geometry, const IslandPlacement &placement,
                  Shader blur_shader, int texture_size_location, int blur_radius_location) {
  if (visual.opacity <= 0.001F) {
    return;
  }

  const Vector2 texture_size{static_cast<float>(texture.texture.width),
                             static_cast<float>(texture.texture.height)};
  SetShaderValue(blur_shader, texture_size_location, &texture_size, SHADER_UNIFORM_VEC2);
  SetShaderValue(blur_shader, blur_radius_location, &visual.blur, SHADER_UNIFORM_FLOAT);

  const float width = texture_size.x * visual.scale;
  const float height = texture_size.y * visual.scale;
  const Rectangle source{0.0F, 0.0F, texture_size.x, -texture_size.y};
  const Rectangle destination{
      placement.x + ((geometry.width - width) / 2.0F),
      placement.y + ((geometry.height - height) / 2.0F),
      width,
      height,
  };

  rlSetBlendFactorsSeparate(RL_ONE, RL_ONE_MINUS_SRC_ALPHA, RL_ZERO, RL_ONE, RL_FUNC_ADD,
                            RL_FUNC_ADD);
  BeginBlendMode(BLEND_CUSTOM_SEPARATE);
  BeginShaderMode(blur_shader);
  DrawTexturePro(texture.texture, source, destination, Vector2{}, 0.0F,
                 ColorAlpha(WHITE, std::clamp(visual.opacity, 0.0F, 1.0F)));
  EndShaderMode();
  EndBlendMode();
}

[[nodiscard]] float mix(float from, float to, float progress) {
  return from + ((to - from) * progress);
}

[[nodiscard]] int mix(int from, int to, float progress) {
  return static_cast<int>(
      std::lround(mix(static_cast<float>(from), static_cast<float>(to), progress)));
}

[[nodiscard]] std::uint8_t mix(std::uint8_t from, std::uint8_t to, float progress) {
  return static_cast<std::uint8_t>(
      std::clamp(mix(static_cast<float>(from), static_cast<float>(to), progress), 0.0F, 255.0F));
}

[[nodiscard]] Rgba mix(Rgba from, Rgba to, float progress) {
  return Rgba{mix(from.red, to.red, progress), mix(from.green, to.green, progress),
              mix(from.blue, to.blue, progress), mix(from.alpha, to.alpha, progress)};
}

[[nodiscard]] RoundedView interpolate(const RoundedView &compact, const RoundedView &expanded,
                                      float progress) {
  return RoundedView{
      .bounds = {0, 0, mix(compact.bounds.width, expanded.bounds.width, progress),
                 mix(compact.bounds.height, expanded.bounds.height, progress)},
      .radius = mix(compact.radius, expanded.radius, progress),
      .border = mix(compact.border, expanded.border, progress),
      .surface = mix(compact.surface, expanded.surface, progress),
      .border_color = mix(compact.border_color, expanded.border_color, progress),
      .shadow =
          ViewShadow{
              .offset_x = mix(compact.shadow.offset_x, expanded.shadow.offset_x, progress),
              .offset_y = mix(compact.shadow.offset_y, expanded.shadow.offset_y, progress),
              .blur = mix(compact.shadow.blur, expanded.shadow.blur, progress),
              .spread = mix(compact.shadow.spread, expanded.shadow.spread, progress),
              .color = mix(compact.shadow.color, expanded.shadow.color, progress),
          },
  };
}

[[nodiscard]] IslandGeometry geometry(const RoundedView &view) {
  return IslandGeometry{static_cast<float>(view.bounds.width),
                        static_cast<float>(view.bounds.height), static_cast<float>(view.radius)};
}

struct RenderedContext {
  ContextKey key;
  std::uint64_t revision;
  LayoutPlan compact;
  std::optional<LayoutPlan> expanded;
  RenderTexture2D compact_content;
  std::optional<RenderTexture2D> expanded_content;
};

void unload(RenderedContext &context) {
  if (context.expanded_content) {
    UnloadRenderTexture(*context.expanded_content);
  }
  UnloadRenderTexture(context.compact_content);
}

[[nodiscard]] std::expected<RenderTexture2D, std::string>
render_content(const LayoutPlan &plan, const RaylibPainter &painter) {
  RenderTexture2D texture =
      LoadRenderTexture(std::max(1, plan.view.bounds.width), std::max(1, plan.view.bounds.height));
  if (!IsRenderTextureValid(texture)) {
    return std::unexpected("could not allocate a context render texture");
  }
  BeginTextureMode(texture);
  ClearBackground(BLANK);
  auto drawn = painter.draw_content(plan);
  EndTextureMode();
  if (!drawn) {
    const std::string message = drawn.error().message;
    UnloadRenderTexture(texture);
    return std::unexpected(message);
  }
  SetTextureFilter(texture.texture, TEXTURE_FILTER_BILINEAR);
  return texture;
}

[[nodiscard]] std::expected<RenderedContext, std::string>
render_context(const PublishedContext &context, std::uint64_t revision, const Theme &theme,
               const RaylibFontBook &fonts) {
  auto compact = layout_scene(context.compact, theme, ViewMode::compact, fonts);
  if (!compact) {
    return std::unexpected(compact.error().path + ": " + compact.error().message);
  }
  std::optional<LayoutPlan> expanded;
  if (context.expanded) {
    auto candidate = layout_scene(*context.expanded, theme, ViewMode::expanded, fonts);
    if (!candidate) {
      return std::unexpected(candidate.error().path + ": " + candidate.error().message);
    }
    expanded = std::move(*candidate);
  }
  const RaylibPainter painter{fonts};
  auto compact_content = render_content(*compact, painter);
  if (!compact_content) {
    return std::unexpected(compact_content.error());
  }
  std::optional<RenderTexture2D> expanded_content;
  if (expanded) {
    auto candidate = render_content(*expanded, painter);
    if (!candidate) {
      UnloadRenderTexture(*compact_content);
      return std::unexpected(candidate.error());
    }
    expanded_content = *candidate;
  }
  return RenderedContext{
      .key = context.key,
      .revision = revision,
      .compact = std::move(*compact),
      .expanded = std::move(expanded),
      .compact_content = *compact_content,
      .expanded_content = expanded_content,
  };
}

[[nodiscard]] std::string environment_or(const char *name, std::string fallback) {
  const char *value = std::getenv(name);
  return value != nullptr && *value != '\0' ? std::string{value} : std::move(fallback);
}

void log_supervisor_event(const SupervisorEvent &event) {
  std::visit(
      [](const auto &typed_event) {
        using Event = std::decay_t<decltype(typed_event)>;
        if constexpr (std::is_same_v<Event, StderrLogEvent>) {
          std::cerr << '[' << typed_event.instance_id << "] " << typed_event.line << '\n';
        } else if constexpr (std::is_same_v<Event, ProtocolViolationEvent>) {
          std::cerr << '[' << typed_event.instance_id << "] protocol " << typed_event.error.path
                    << ": " << typed_event.error.message << '\n';
        } else if constexpr (std::is_same_v<Event, SupervisorErrorEvent>) {
          std::cerr << '[' << typed_event.instance_id << "] " << typed_event.message << '\n';
        } else if constexpr (std::is_same_v<Event, ModuleMessageEvent>) {
          if (const auto *log = std::get_if<LogMessage>(&typed_event.message)) {
            std::cerr << '[' << typed_event.instance_id << "] " << log->message << '\n';
          }
        }
      },
      event);
}

} // namespace

Application::Application(RuntimeBootstrap bootstrap, ApplicationConfig config)
    : bootstrap_(std::move(bootstrap)), config_(std::move(config)) {}

int Application::run() {
  const auto &compact_theme = bootstrap_.theme.views().compact;
  const IslandGeometry initial_geometry{static_cast<float>(compact_theme.min_width),
                                        static_cast<float>(compact_theme.min_height),
                                        static_cast<float>(compact_theme.radius)};
  Window window{config_, initial_geometry};
  if (!Window::is_ready()) {
    std::cerr << "Failed to initialize the raylib window\n";
    return EXIT_FAILURE;
  }
  SetTargetFPS(config_.target_fps);

  auto fonts = RaylibFontBook::load(bootstrap_.theme, bootstrap_.asset_root);
  if (!fonts) {
    std::cerr << fonts.error().message << '\n';
    return EXIT_FAILURE;
  }
  const RaylibPainter painter{*fonts};
  std::optional<X11WindowHost> host;
  auto created_host = X11WindowHost::create(GetWindowHandle());
  if (created_host) {
    host.emplace(std::move(*created_host));
  } else {
    std::cerr << created_host.error().message << '\n';
  }

  ModuleSupervisor supervisor;
  RuntimeCoordinator runtime{bootstrap_.config};
  const std::string locale = environment_or("LC_ALL", environment_or("LANG", "C"));
  const std::string timezone = environment_or("TZ", "UTC");
  for (const auto &module : bootstrap_.config.modules) {
    if (!module.enabled) {
      continue;
    }
    auto started = supervisor.start(make_module_start_request(module, locale, timezone));
    if (!started) {
      std::cerr << '[' << module.id << "] module start request failed\n";
    }
  }

  const Shader blur_shader = LoadShaderFromMemory(nullptr, content_blur_shader);
  const int texture_size_location = GetShaderLocation(blur_shader, "textureSize");
  const int blur_radius_location = GetShaderLocation(blur_shader, "blurRadius");
  OverlayInteraction interaction;
  IslandMode mode = interaction.mode();
  SpringProgress spring;
  ContentCrossfade content_crossfade;
  IslandGeometry current = initial_geometry;
  const IslandPlacement placement{0.0F, 0.0F};
  X11Monitor monitor = raylib_monitor_fallback();
  std::optional<RenderedContext> rendered;
  bool visible = false;

  const auto refresh_monitor = [&] {
    if (!host) {
      monitor = raylib_monitor_fallback();
      return;
    }
    auto selected = host->select_output(bootstrap_.config.monitor);
    if (!selected) {
      std::cerr << selected.error().message << '\n';
      return;
    }
    monitor = std::move(selected->monitor);
    if (selected->used_fallback) {
      std::cerr << "X11 output '" << bootstrap_.config.monitor << "' is unavailable; using '"
                << monitor.name << "'\n";
    }
  };

  int native_width = std::max(1, static_cast<int>(std::lround(current.width)));
  int native_height = std::max(1, static_cast<int>(std::lround(current.height)));
  std::optional<X11WindowPlacement> native_position;
  const auto apply_native_geometry = [&] {
    const int next_width = std::max(1, static_cast<int>(std::lround(current.width)));
    const int next_height = std::max(1, static_cast<int>(std::lround(current.height)));
    if (next_width != native_width || next_height != native_height) {
      SetWindowSize(next_width, next_height);
      native_width = next_width;
      native_height = next_height;
    }
    auto positioned = place_on_monitor(monitor, native_width, native_height, config_.top_margin);
    if (!positioned) {
      std::cerr << positioned.error().message << '\n';
      return;
    }
    if (!native_position || native_position->x != positioned->x ||
        native_position->y != positioned->y) {
      SetWindowPosition(positioned->x, positioned->y);
      native_position = *positioned;
    }
    if (host) {
      if (auto shaped = host->apply_shape(current); !shaped) {
        std::cerr << shaped.error().message << '\n';
      }
    }
  };
  refresh_monitor();
  apply_native_geometry();

  while (!WindowShouldClose()) {
    const float delta_seconds = GetFrameTime();
    const MonotonicTime now = std::chrono::steady_clock::now();
    for (const auto &event : supervisor.drain_events()) {
      log_supervisor_event(event);
      if (auto consumed = runtime.consume(event); !consumed) {
        std::cerr << '[' << consumed.error().instance_id << "] " << consumed.error().message
                  << '\n';
      }
    }

    auto selection = runtime.active(now);
    const bool changed =
        selection.context != nullptr && (!rendered || rendered->key != selection.context->key ||
                                         rendered->revision != selection.revision);
    if (changed) {
      if (interaction.dismiss(OverlayDismissal::focus_lost) && host) {
        if (auto left = host->leave_expanded(false); !left) {
          std::cerr << left.error().message << '\n';
        }
      }
      auto candidate =
          render_context(*selection.context, selection.revision, bootstrap_.theme, *fonts);
      if (!candidate) {
        std::cerr << '[' << selection.context->key.instance_id << "] layout: " << candidate.error()
                  << '\n';
        runtime.reject(selection.context->key);
        if (rendered) {
          unload(*rendered);
          rendered.reset();
        }
      } else {
        if (rendered) {
          unload(*rendered);
        }
        rendered.emplace(std::move(*candidate));
        interaction = OverlayInteraction{};
        mode = IslandMode::compact;
        spring = SpringProgress{};
        content_crossfade = ContentCrossfade{};
        current = geometry(rendered->compact.view);
      }
    } else if (selection.context == nullptr && rendered) {
      if (interaction.dismiss(OverlayDismissal::focus_lost) && host) {
        if (auto left = host->leave_expanded(false); !left) {
          std::cerr << left.error().message << '\n';
        }
      }
      unload(*rendered);
      rendered.reset();
      if (visible) {
        Window::hide();
        visible = false;
      }
    }

    bool topology_changed = false;
    if (host) {
      auto events = host->poll_events();
      if (!events) {
        std::cerr << events.error().message << '\n';
      } else {
        for (const auto &event : *events) {
          if (event.kind == X11WindowEventKind::inside_press && rendered && rendered->expanded &&
              interaction.pointer_pressed(event.button, true)) {
            if (auto entered = host->enter_expanded(event.timestamp); !entered) {
              std::cerr << entered.error().message << '\n';
              static_cast<void>(interaction.dismiss(OverlayDismissal::focus_lost));
            }
          } else if (event.kind == X11WindowEventKind::outside_press &&
                     interaction.dismiss(OverlayDismissal::outside_press)) {
            if (auto left = host->leave_expanded(false); !left) {
              std::cerr << left.error().message << '\n';
            }
          } else if (event.kind == X11WindowEventKind::focus_lost &&
                     interaction.dismiss(OverlayDismissal::focus_lost)) {
            if (auto left = host->leave_expanded(false); !left) {
              std::cerr << left.error().message << '\n';
            }
          } else if (event.kind == X11WindowEventKind::topology_changed) {
            topology_changed = true;
          }
        }
      }
    }
    if (topology_changed) {
      refresh_monitor();
    }
    if (!host && rendered && rendered->expanded && interaction.mode() == IslandMode::compact &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      static_cast<void>(interaction.pointer_pressed(PointerButton::primary, true));
    }
    if (interaction.mode() == IslandMode::expanded && IsKeyPressed(KEY_ESCAPE) &&
        interaction.dismiss(OverlayDismissal::escape) && host) {
      if (auto left = host->leave_expanded(true); !left) {
        std::cerr << left.error().message << '\n';
      }
    }

    const IslandMode next_mode = interaction.mode();
    if (next_mode != mode) {
      mode = next_mode;
      spring.set_target(mode == IslandMode::expanded ? 1.0F : 0.0F);
      content_crossfade.set_mode(mode);
    }
    for (const auto &update : runtime.visibility_updates(now, mode)) {
      if (auto sent = supervisor.send(update.instance_id, VisibilityMessage{update.visibility});
          !sent) {
        std::cerr << '[' << update.instance_id << "] visibility update failed\n";
      }
    }

    spring.update(delta_seconds);
    content_crossfade.update(delta_seconds);
    if (rendered) {
      const RoundedView &expanded_view =
          rendered->expanded ? rendered->expanded->view : rendered->compact.view;
      const RoundedView surface =
          interpolate(rendered->compact.view, expanded_view, spring.value());
      current = geometry(surface);
      apply_native_geometry();
      BeginDrawing();
      ClearBackground(BLANK);
      if (auto drawn = painter.draw_surface(LayoutPlan{surface, {}}); !drawn) {
        std::cerr << drawn.error().message << '\n';
      }
      draw_content(rendered->compact_content, content_crossfade.compact(), current, placement,
                   blur_shader, texture_size_location, blur_radius_location);
      if (rendered->expanded_content) {
        draw_content(*rendered->expanded_content, content_crossfade.expanded(), current, placement,
                     blur_shader, texture_size_location, blur_radius_location);
      }
      EndDrawing();
      if (!visible) {
        Window::show();
        visible = true;
      }
    } else {
      BeginDrawing();
      ClearBackground(BLANK);
      EndDrawing();
    }
  }

  supervisor.shutdown();
  if (rendered) {
    unload(*rendered);
  }
  UnloadShader(blur_shader);
  return EXIT_SUCCESS;
}

} // namespace gisland
