#include "gisland/application.hpp"
#include "gisland/control_dispatcher.hpp"
#include "gisland/file_watcher.hpp"
#include "gisland/interaction.hpp"
#include "gisland/ipc_server.hpp"
#include "gisland/island.hpp"
#include "gisland/layout.hpp"
#include "gisland/module_supervisor.hpp"
#include "gisland/raylib_renderer.hpp"
#include "gisland/reload.hpp"
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
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
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
                   FLAG_WINDOW_ALWAYS_RUN | FLAG_WINDOW_HIDDEN | FLAG_WINDOW_UNFOCUSED);
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

[[nodiscard]] std::vector<std::filesystem::path>
reload_watch_paths(const RuntimeBootstrap &bootstrap) {
  std::vector<std::filesystem::path> paths{bootstrap.config_path, bootstrap.theme_path};
  paths.insert(paths.end(), bootstrap.manifest_paths.begin(), bootstrap.manifest_paths.end());
  return paths;
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

[[nodiscard]] IslandCanvasSize canvas_for(const LayoutPlan &compact,
                                          const std::optional<LayoutPlan> &expanded) {
  RectInsets insets = shadow_insets(compact.view);
  if (expanded) {
    const RectInsets expanded_insets = shadow_insets(expanded->view);
    insets.left = std::max(insets.left, expanded_insets.left);
    insets.top = std::max(insets.top, expanded_insets.top);
    insets.right = std::max(insets.right, expanded_insets.right);
    insets.bottom = std::max(insets.bottom, expanded_insets.bottom);
  }
  const float surface_width =
      static_cast<float>(std::max(compact.view.bounds.width, expanded ? expanded->view.bounds.width
                                                                      : compact.view.bounds.width));
  const float surface_height = static_cast<float>(
      std::max(compact.view.bounds.height,
               expanded ? expanded->view.bounds.height : compact.view.bounds.height));
  return IslandCanvasSize{
      .width = static_cast<float>(insets.left + insets.right) + surface_width,
      .height = static_cast<float>(insets.top + insets.bottom) + surface_height,
      .surface_x = static_cast<float>(insets.left),
      .surface_y = static_cast<float>(insets.top),
      .surface_width = surface_width,
      .surface_height = surface_height,
  };
}

[[nodiscard]] IslandCanvasSize combine_canvases(const IslandCanvasSize &left,
                                                const IslandCanvasSize &right) {
  const auto surface_width = [](const IslandCanvasSize &canvas) {
    return canvas.surface_width > 0.0F ? canvas.surface_width : canvas.width;
  };
  const auto surface_height = [](const IslandCanvasSize &canvas) {
    return canvas.surface_height > 0.0F ? canvas.surface_height : canvas.height;
  };
  const float left_inset = std::max(left.surface_x, right.surface_x);
  const float top_inset = std::max(left.surface_y, right.surface_y);
  const float width = std::max(surface_width(left), surface_width(right));
  const float height = std::max(surface_height(left), surface_height(right));
  const float right_inset = std::max(left.width - left.surface_x - surface_width(left),
                                     right.width - right.surface_x - surface_width(right));
  const float bottom_inset = std::max(left.height - left.surface_y - surface_height(left),
                                      right.height - right.surface_y - surface_height(right));
  return IslandCanvasSize{left_inset + width + right_inset,
                          top_inset + height + bottom_inset,
                          left_inset,
                          top_inset,
                          width,
                          height};
}

[[nodiscard]] X11CanvasGeometry native_geometry(const IslandCanvasSize &canvas) {
  const float surface_width = canvas.surface_width > 0.0F ? canvas.surface_width : canvas.width;
  return X11CanvasGeometry{
      std::max(1, static_cast<int>(std::lround(canvas.width))),
      std::max(1, static_cast<int>(std::lround(canvas.height))),
      static_cast<int>(std::lround(canvas.surface_x)),
      static_cast<int>(std::lround(canvas.surface_y)),
      std::max(1, static_cast<int>(std::lround(surface_width))),
  };
}

struct RenderedContext {
  std::optional<ContextKey> compact_key;
  std::optional<ContextKey> expanded_key;
  std::uint64_t compact_revision;
  std::uint64_t expanded_revision;
  LayoutPlan compact;
  std::optional<LayoutPlan> expanded;
  RenderTexture2D compact_content;
  std::optional<RenderTexture2D> expanded_content;
};

struct RenderContextError {
  ViewSlot slot;
  std::string message;
};

[[nodiscard]] RoundedView visible_surface(const RenderedContext &context, float mode_progress) {
  const RoundedView &expanded = context.expanded ? context.expanded->view : context.compact.view;
  return interpolate(context.compact.view, expanded, mode_progress);
}

[[nodiscard]] ContentVisual with_opacity(ContentVisual visual, float multiplier) {
  visual.opacity *= std::clamp(multiplier, 0.0F, 1.0F);
  return visual;
}

void unload(RenderedContext &context) {
  if (context.expanded_content) {
    UnloadRenderTexture(*context.expanded_content);
  }
  UnloadRenderTexture(context.compact_content);
}

[[nodiscard]] std::expected<RenderTexture2D, std::string>
snapshot_content(const std::optional<RenderTexture2D> &outgoing, float outgoing_opacity,
                 const RenderedContext &incoming, float incoming_opacity,
                 const ContentCrossfade &mode_crossfade, const IslandGeometry &geometry,
                 Shader blur_shader, int texture_size_location, int blur_radius_location) {
  RenderTexture2D texture =
      LoadRenderTexture(std::max(1, static_cast<int>(std::lround(geometry.width))),
                        std::max(1, static_cast<int>(std::lround(geometry.height))));
  if (!IsRenderTextureValid(texture)) {
    return std::unexpected("could not allocate a context transition snapshot");
  }

  BeginTextureMode(texture);
  ClearBackground(BLANK);
  const IslandPlacement origin{};
  if (outgoing) {
    draw_content(*outgoing, ContentVisual{outgoing_opacity, 0.0F, 1.0F}, geometry, origin,
                 blur_shader, texture_size_location, blur_radius_location);
  }
  draw_content(incoming.compact_content, with_opacity(mode_crossfade.compact(), incoming_opacity),
               geometry, origin, blur_shader, texture_size_location, blur_radius_location);
  if (incoming.expanded_content) {
    draw_content(*incoming.expanded_content,
                 with_opacity(mode_crossfade.expanded(), incoming_opacity), geometry, origin,
                 blur_shader, texture_size_location, blur_radius_location);
  }
  EndTextureMode();
  SetTextureFilter(texture.texture, TEXTURE_FILTER_BILINEAR);
  return texture;
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

[[nodiscard]] std::expected<RenderedContext, RenderContextError>
render_context(const RuntimeSelection &compact_selection,
               const RuntimeSelection &expanded_selection, const Theme &theme,
               const RaylibFontBook &fonts, const PangoTextBook &rich_text) {
  if ((compact_selection.context == nullptr || compact_selection.scene == nullptr) &&
      (expanded_selection.context == nullptr || expanded_selection.scene == nullptr)) {
    return std::unexpected(
        RenderContextError{ViewSlot::compact, "no view contribution is available"});
  }

  const auto render_slot =
      [&](const RuntimeSelection &selection,
          ViewMode mode) -> std::expected<std::pair<LayoutPlan, RenderTexture2D>, std::string> {
    auto plan = layout_scene(*selection.scene, theme, mode, fonts, rich_text);
    if (!plan) {
      return std::unexpected(plan.error().path + ": " + plan.error().message);
    }
    auto images = RaylibImageBook::load(selection.context->resources);
    if (!images) {
      return std::unexpected(images.error().message);
    }
    if (auto prepared = images->prepare(*plan); !prepared) {
      return std::unexpected(prepared.error().message);
    }
    auto rich_textures = RaylibRichTextBook::load(rich_text, selection.context->resources);
    if (!rich_textures) {
      return std::unexpected(rich_textures.error().message);
    }
    if (auto prepared = rich_textures->prepare(*plan); !prepared) {
      return std::unexpected(prepared.error().message);
    }
    const RaylibPainter slot_painter{fonts, *images, *rich_textures};
    auto content = render_content(*plan, slot_painter);
    if (!content) {
      return std::unexpected(content.error());
    }
    return std::pair<LayoutPlan, RenderTexture2D>{std::move(*plan), *content};
  };

  std::optional<std::pair<LayoutPlan, RenderTexture2D>> compact;
  if (compact_selection.context != nullptr && compact_selection.scene != nullptr) {
    auto candidate = render_slot(compact_selection, ViewMode::compact);
    if (!candidate) {
      return std::unexpected(RenderContextError{ViewSlot::compact, candidate.error()});
    }
    compact = std::move(*candidate);
  }
  std::optional<std::pair<LayoutPlan, RenderTexture2D>> expanded;
  if (expanded_selection.context != nullptr && expanded_selection.scene != nullptr) {
    auto candidate = render_slot(expanded_selection, ViewMode::expanded);
    if (!candidate) {
      if (compact) {
        UnloadRenderTexture(compact->second);
      }
      return std::unexpected(RenderContextError{ViewSlot::expanded, candidate.error()});
    }
    expanded = std::move(*candidate);
  }

  std::optional<LayoutPlan> expanded_plan;
  std::optional<RenderTexture2D> expanded_content;
  std::optional<ContextKey> expanded_key;
  if (expanded) {
    expanded_plan = std::move(expanded->first);
    expanded_content = expanded->second;
    expanded_key = expanded_selection.context->key;
  }
  if (!compact) {
    RenderTexture2D blank = LoadRenderTexture(std::max(1, expanded_plan->view.bounds.width),
                                              std::max(1, expanded_plan->view.bounds.height));
    if (!IsRenderTextureValid(blank)) {
      UnloadRenderTexture(*expanded_content);
      return std::unexpected(RenderContextError{
          ViewSlot::expanded, "could not allocate an empty compact render texture"});
    }
    BeginTextureMode(blank);
    ClearBackground(BLANK);
    EndTextureMode();
    SetTextureFilter(blank.texture, TEXTURE_FILTER_BILINEAR);
    compact = std::pair<LayoutPlan, RenderTexture2D>{*expanded_plan, blank};
  }
  return RenderedContext{
      .compact_key = compact_selection.context == nullptr
                         ? std::nullopt
                         : std::optional{compact_selection.context->key},
      .expanded_key = std::move(expanded_key),
      .compact_revision = compact_selection.revision,
      .expanded_revision = expanded_selection.revision,
      .compact = std::move(compact->first),
      .expanded = std::move(expanded_plan),
      .compact_content = compact->second,
      .expanded_content = expanded_content,
  };
}

[[nodiscard]] std::string environment_or(const char *name, std::string fallback) {
  const char *value = std::getenv(name);
  return value != nullptr && *value != '\0' ? std::string{value} : std::move(fallback);
}

[[nodiscard]] std::string runtime_locale() {
  return environment_or("LC_ALL", environment_or("LC_TIME", environment_or("LANG", "C")));
}

[[nodiscard]] std::string runtime_timezone() {
  if (const char *timezone = std::getenv("TZ"); timezone != nullptr && *timezone != '\0') {
    return timezone;
  }
  try {
    return std::string{std::chrono::current_zone()->name()};
  } catch (const std::runtime_error &) {
    return "UTC";
  }
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
          } else if (const auto *result = std::get_if<ActionResultMessage>(&typed_event.message)) {
            std::cerr << '[' << typed_event.instance_id << "] action '" << result->action_id << "' "
                      << (result->accepted ? "accepted" : "rejected");
            if (result->message) {
              std::cerr << ": " << *result->message;
            }
            std::cerr << '\n';
          }
        }
      },
      event);
}

} // namespace

Application::Application(RuntimeBootstrap bootstrap, ApplicationConfig config)
    : bootstrap_(std::move(bootstrap)), config_(std::move(config)) {}

int Application::run() {
  const char *runtime_directory = std::getenv("XDG_RUNTIME_DIR");
  if (runtime_directory == nullptr || *runtime_directory == '\0') {
    std::cerr << "XDG_RUNTIME_DIR is unset\n";
    return EXIT_FAILURE;
  }
  auto ipc = IpcServer::create(runtime_directory);
  if (!ipc) {
    std::cerr << ipc.error().message << '\n';
    return EXIT_FAILURE;
  }

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
  auto rich_text = PangoTextBook::load(bootstrap_.theme, bootstrap_.asset_root);
  if (!rich_text) {
    std::cerr << rich_text.error().message << '\n';
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
  const std::string locale = runtime_locale();
  const std::string timezone = runtime_timezone();
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
  OverlayModeController mode_controller{bootstrap_.config.interaction.hover_exit};
  InteractionController controls;
  IslandMode mode = mode_controller.mode();
  SpringProgress spring;
  ContentCrossfade content_crossfade;
  IslandGeometry current = initial_geometry;
  IslandCanvasSize canvas{initial_geometry.width, initial_geometry.height};
  IslandPlacement placement = place_at_top_center(current, canvas);
  X11Monitor monitor = raylib_monitor_fallback();
  std::optional<RenderedContext> rendered;
  std::optional<RenderTexture2D> outgoing_content;
  std::optional<RoundedView> current_surface;
  std::optional<RoundedView> transition_source_surface;
  std::optional<RoundedView> transition_target_surface;
  ContextTransition context_transition;
  bool visible = false;
  bool actions_ready = false;

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

  int native_width = std::max(1, static_cast<int>(std::lround(canvas.width)));
  int native_height = std::max(1, static_cast<int>(std::lround(canvas.height)));
  std::optional<X11WindowPlacement> native_position;
  const auto apply_native_canvas = [&] {
    const X11CanvasGeometry geometry = native_geometry(canvas);
    const int next_width = geometry.width;
    const int next_height = geometry.height;
    if (next_width != native_width || next_height != native_height) {
      SetWindowSize(next_width, next_height);
      native_width = next_width;
      native_height = next_height;
    }
    auto positioned = place_on_monitor(monitor, geometry, config_.top_margin);
    if (!positioned) {
      std::cerr << positioned.error().message << '\n';
      return;
    }
    if (!native_position || native_position->x != positioned->x ||
        native_position->y != positioned->y) {
      SetWindowPosition(positioned->x, positioned->y);
      native_position = *positioned;
    }
  };
  refresh_monitor();
  apply_native_canvas();

  const auto clear_outgoing = [&] {
    if (outgoing_content) {
      UnloadRenderTexture(*outgoing_content);
      outgoing_content.reset();
    }
  };

  const auto replace_rendered = [&](RenderedContext candidate, bool preserve_expanded,
                                    const AnimationStyle &animation) {
    std::optional<RenderTexture2D> snapshot;
    if (rendered && current_surface && animation.context_change_ms.count() > 0) {
      const auto transition_visual = context_transition.visual();
      auto captured =
          snapshot_content(outgoing_content, transition_visual.outgoing_opacity, *rendered,
                           transition_visual.incoming_opacity, content_crossfade, current,
                           blur_shader, texture_size_location, blur_radius_location);
      if (captured) {
        snapshot = *captured;
      } else {
        std::cerr << captured.error() << '\n';
      }
    }

    clear_outgoing();
    if (rendered) {
      unload(*rendered);
    }
    rendered.emplace(std::move(candidate));
    actions_ready = false;
    if (!preserve_expanded) {
      mode_controller = OverlayModeController{bootstrap_.config.interaction.hover_exit};
      mode = IslandMode::compact;
      spring = SpringProgress{};
      content_crossfade = ContentCrossfade{};
    }

    const RoundedView target = visible_surface(*rendered, spring.value());
    const auto target_canvas = canvas_for(rendered->compact, rendered->expanded);
    canvas = combine_canvases(canvas, target_canvas);
    if (snapshot && current_surface) {
      outgoing_content = *snapshot;
      transition_source_surface = *current_surface;
      transition_target_surface = target;
      context_transition.start(current, geometry(target), animation.context_change_ms,
                               animation.easing);
    } else {
      transition_source_surface.reset();
      transition_target_surface.reset();
      context_transition.start(geometry(target), geometry(target), std::chrono::milliseconds{0},
                               animation.easing);
      current_surface = target;
      current = geometry(target);
    }
    placement = place_at_top_center(current, canvas);
    apply_native_canvas();
  };

  std::optional<FileWatcher> watcher;
  if (auto created = FileWatcher::create(reload_watch_paths(bootstrap_)); created) {
    watcher.emplace(std::move(*created));
  } else {
    std::cerr << "automatic reload disabled: " << created.error() << '\n';
  }
  ReloadDebouncer reload_debouncer{std::chrono::milliseconds{100}};

  const auto reload_application = [&](MonotonicTime now) -> std::expected<void, std::string> {
    auto candidate_bootstrap = load_reload_candidate(bootstrap_);
    if (!candidate_bootstrap) {
      return std::unexpected(candidate_bootstrap.error().path.string() + ": " +
                             candidate_bootstrap.error().message);
    }
    auto plan = plan_reload(bootstrap_.config, candidate_bootstrap->config, locale, timezone);
    if (!plan) {
      return std::unexpected(plan.error().message);
    }
    auto prepared_runtime = runtime.prepare_reload(*plan);
    if (!prepared_runtime) {
      return std::unexpected(prepared_runtime.error().message);
    }
    auto candidate_fonts =
        RaylibFontBook::load(candidate_bootstrap->theme, candidate_bootstrap->asset_root);
    if (!candidate_fonts) {
      return std::unexpected(candidate_fonts.error().message);
    }
    auto candidate_rich_text =
        PangoTextBook::load(candidate_bootstrap->theme, candidate_bootstrap->asset_root);
    if (!candidate_rich_text) {
      return std::unexpected(candidate_rich_text.error().message);
    }

    X11Monitor candidate_monitor = monitor;
    if (host) {
      auto selected = host->select_output(candidate_bootstrap->config.monitor);
      if (!selected) {
        return std::unexpected(selected.error().message);
      }
      candidate_monitor = std::move(selected->monitor);
    }

    const auto make_selection = [&](ViewSlot slot) {
      const auto *context = prepared_runtime->arbiter.active(slot, now);
      const SceneNode *scene = nullptr;
      if (context != nullptr) {
        const auto &contribution = slot == ViewSlot::compact ? context->compact : context->expanded;
        scene = contribution ? &*contribution : nullptr;
      }
      return RuntimeSelection{
          context, context == nullptr ? prepared_runtime->revision : context->revision, scene};
    };
    const auto candidate_compact = make_selection(ViewSlot::compact);
    const auto candidate_expanded = make_selection(ViewSlot::expanded);
    std::optional<RenderedContext> candidate_rendered;
    if (candidate_compact.context != nullptr || candidate_expanded.context != nullptr) {
      auto candidate =
          render_context(candidate_compact, candidate_expanded, candidate_bootstrap->theme,
                         *candidate_fonts, *candidate_rich_text);
      if (!candidate) {
        return std::unexpected(candidate.error().message);
      }
      candidate_rendered.emplace(std::move(*candidate));
    }
    const auto candidate_canvas =
        candidate_rendered
            ? canvas_for(candidate_rendered->compact, candidate_rendered->expanded)
            : IslandCanvasSize{
                  static_cast<float>(candidate_bootstrap->theme.views().compact.min_width),
                  static_cast<float>(candidate_bootstrap->theme.views().compact.min_height)};
    if (auto positioned = place_on_monitor(candidate_monitor, native_geometry(candidate_canvas),
                                           config_.top_margin);
        !positioned) {
      if (candidate_rendered) {
        unload(*candidate_rendered);
      }
      return std::unexpected(positioned.error().message);
    }

    if (auto queued = supervisor.reconfigure(std::move(plan->supervisor)); !queued) {
      if (candidate_rendered) {
        unload(*candidate_rendered);
      }
      return std::unexpected("module reconfiguration could not be queued");
    }

    runtime.commit_reload(std::move(*prepared_runtime));
    *fonts = std::move(*candidate_fonts);
    *rich_text = std::move(*candidate_rich_text);
    bootstrap_ = std::move(*candidate_bootstrap);
    monitor = std::move(candidate_monitor);
    mode_controller.set_exit_tolerance(bootstrap_.config.interaction.hover_exit);
    if (mode_controller.mode() == IslandMode::expanded &&
        (!candidate_rendered || !candidate_rendered->expanded)) {
      mode_controller.close();
    }
    if (candidate_rendered) {
      const bool preserve_expanded = mode_controller.mode() == IslandMode::expanded &&
                                     candidate_rendered->expanded.has_value();
      replace_rendered(std::move(*candidate_rendered), preserve_expanded,
                       bootstrap_.theme.animation());
    } else {
      clear_outgoing();
      if (rendered) {
        unload(*rendered);
        rendered.reset();
      }
      current_surface.reset();
      transition_source_surface.reset();
      transition_target_surface.reset();
      const auto &compact = bootstrap_.theme.views().compact;
      current = IslandGeometry{static_cast<float>(compact.min_width),
                               static_cast<float>(compact.min_height),
                               static_cast<float>(compact.radius)};
      canvas = candidate_canvas;
      placement = place_at_top_center(current, canvas);
      apply_native_canvas();
    }
    if (watcher) {
      if (auto replaced = watcher->replace_paths(reload_watch_paths(bootstrap_)); !replaced) {
        std::cerr << "automatic reload disabled: " << replaced.error() << '\n';
        watcher.reset();
      }
    }
    return {};
  };

  ControlDispatcher dispatcher{runtime, mode_controller,
                               [&supervisor](std::string instance_id, std::uint64_t generation) {
                                 return supervisor.restart(std::move(instance_id), generation);
                               },
                               ipc->socket_path(),
                               [&](MonotonicTime now) {
                                 auto reloaded = reload_application(now);
                                 if (reloaded) {
                                   reload_debouncer.clear();
                                 }
                                 return reloaded;
                               }};

  while (!WindowShouldClose()) {
    const float delta_seconds = GetFrameTime();
    const MonotonicTime now = std::chrono::steady_clock::now();
    if (watcher) {
      auto changed_paths = watcher->poll();
      if (!changed_paths) {
        std::cerr << "automatic reload disabled: " << changed_paths.error() << '\n';
        watcher.reset();
        reload_debouncer.clear();
      } else if (*changed_paths) {
        reload_debouncer.observe(now);
      }
    }
    if (reload_debouncer.consume_due(now)) {
      if (auto reloaded = reload_application(now); !reloaded) {
        std::cerr << "automatic reload rejected: " << reloaded.error() << '\n';
      }
    }
    for (const auto &event : supervisor.drain_events()) {
      log_supervisor_event(event);
      if (const auto *completed = std::get_if<RestartCompletedEvent>(&event)) {
        dispatcher.consume(*completed);
      }
      if (auto consumed = runtime.consume(event); !consumed) {
        std::cerr << '[' << consumed.error().instance_id << "] " << consumed.error().message
                  << '\n';
      }
    }

    static_cast<void>(runtime.active(now));
    ipc->advance(now, [&dispatcher, now](const ControlCommand &command) {
      return dispatcher.dispatch(command, now);
    });
    auto selection = runtime.selections(now);
    const bool expanded_changed =
        selection.expanded.context != nullptr
            ? (!rendered || rendered->expanded_key != selection.expanded.context->key ||
               rendered->expanded_revision != selection.expanded.revision)
            : rendered && rendered->expanded_key.has_value();
    const std::optional<ContextKey> compact_key =
        selection.compact.context == nullptr
            ? std::nullopt
            : std::optional<ContextKey>{selection.compact.context->key};
    const bool changed =
        (selection.compact.context != nullptr || selection.expanded.context != nullptr) &&
        (!rendered || rendered->compact_key != compact_key ||
         rendered->compact_revision != selection.compact.revision || expanded_changed);
    if (changed) {
      const bool preserve_expanded =
          mode_controller.mode() == IslandMode::expanded && selection.expanded.context != nullptr;
      auto candidate = render_context(selection.compact, selection.expanded, bootstrap_.theme,
                                      *fonts, *rich_text);
      if (!candidate) {
        const RuntimeSelection &rejected =
            candidate.error().slot == ViewSlot::compact ? selection.compact : selection.expanded;
        std::cerr << '[' << rejected.context->key.instance_id
                  << "] layout: " << candidate.error().message << '\n';
        runtime.reject(rejected.context->key, now);
      } else {
        if (selection.compact.context != nullptr) {
          runtime.accept(selection.compact.context->key);
        }
        if (selection.expanded.context != nullptr) {
          runtime.accept(selection.expanded.context->key);
        }
        replace_rendered(std::move(*candidate), preserve_expanded, bootstrap_.theme.animation());
        if (expanded_changed) {
          if (selection.expanded.context != nullptr &&
              selection.expanded.context->presentation.has_value()) {
            mode_controller.set_reveal(true, selection.expanded.context->presentation->duration);
          } else {
            mode_controller.set_reveal(false);
          }
        }
      }
    } else if (selection.compact.context == nullptr && selection.expanded.context == nullptr &&
               rendered) {
      clear_outgoing();
      unload(*rendered);
      rendered.reset();
      current_surface.reset();
      transition_source_surface.reset();
      transition_target_surface.reset();
      mode_controller = OverlayModeController{bootstrap_.config.interaction.hover_exit};
      actions_ready = false;
      if (visible) {
        Window::hide();
        visible = false;
      }
    }

    const auto send_action = [&](const std::optional<std::string> &action) {
      if (!action || !rendered) {
        return;
      }
      const auto &owner =
          mode == IslandMode::expanded ? rendered->expanded_key : rendered->compact_key;
      if (!owner) {
        return;
      }
      if (auto sent = supervisor.send(owner->instance_id,
                                      ActionMessage{.action_id = *action, .value = std::nullopt});
          !sent) {
        std::cerr << '[' << owner->instance_id << "] action delivery failed\n";
      }
    };

    bool topology_changed = false;
    if (host) {
      auto events = host->poll_events();
      if (!events) {
        std::cerr << events.error().message << '\n';
      } else {
        for (const auto &event : *events) {
          if (event.kind == X11WindowEventKind::topology_changed) {
            topology_changed = true;
          }
        }
      }
    }
    if (topology_changed) {
      refresh_monitor();
      apply_native_canvas();
    }
    const Vector2 pointer = GetMousePosition();
    const bool hovered = IsCursorOnScreen() &&
                         CheckCollisionPointRec(pointer, Rectangle{placement.x, placement.y,
                                                                   current.width, current.height});
    std::optional<ButtonDecorationDrawCommand> button_hover;
    if (actions_ready && rendered && rendered->expanded && IsCursorOnScreen()) {
      const int pointer_x = static_cast<int>(std::lround(pointer.x - placement.x));
      const int pointer_y = static_cast<int>(std::lround(pointer.y - placement.y));
      button_hover = controls.pointer_hover(
          *rendered->expanded, pointer_x, pointer_y,
          resolve_theme_color(bootstrap_.theme, bootstrap_.theme.buttons().hover_overlay));
      if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        send_action(controls.pointer_action(*rendered->expanded, pointer_x, pointer_y));
      }
    }

    mode_controller.update(hovered, rendered && rendered->expanded.has_value(), delta_seconds);
    const IslandMode next_mode = mode_controller.mode();
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

    const float animation_delta =
        delta_seconds * static_cast<float>(bootstrap_.config.interaction.animation_speed);
    const bool context_was_active = context_transition.active();
    context_transition.update(animation_delta);
    if (!context_transition.active()) {
      spring.update(animation_delta);
      content_crossfade.update(animation_delta);
    }
    if (context_was_active && !context_transition.active()) {
      clear_outgoing();
      transition_source_surface.reset();
      transition_target_surface.reset();
      if (rendered) {
        canvas = canvas_for(rendered->compact, rendered->expanded);
        apply_native_canvas();
      }
    }
    const ContentVisual expanded_visual = content_crossfade.expanded();
    const bool expanded_settled = !context_transition.active() && mode == IslandMode::expanded &&
                                  spring.value() == 1.0F && expanded_visual.opacity == 1.0F &&
                                  expanded_visual.blur == 0.0F && expanded_visual.scale == 1.0F;
    actions_ready = expanded_settled && rendered && rendered->expanded.has_value();
    if (rendered) {
      const auto transition_visual = context_transition.visual();
      RoundedView surface = visible_surface(*rendered, spring.value());
      if (context_transition.active() && transition_source_surface && transition_target_surface) {
        surface = interpolate(*transition_source_surface, *transition_target_surface,
                              transition_visual.incoming_opacity);
      }
      current_surface = surface;
      current = geometry(surface);
      placement = place_at_top_center(current, canvas);
      surface.bounds.x = static_cast<int>(std::lround(placement.x));
      surface.bounds.y = static_cast<int>(std::lround(placement.y));
      if (host) {
        if (auto shaped = host->apply_shape(current, placement); !shaped) {
          std::cerr << shaped.error().message << '\n';
        }
      }
      BeginDrawing();
      ClearBackground(BLANK);
      if (auto drawn = painter.draw_surface(LayoutPlan{surface, {}, {}}); !drawn) {
        std::cerr << drawn.error().message << '\n';
      }
      if (outgoing_content) {
        draw_content(*outgoing_content,
                     ContentVisual{transition_visual.outgoing_opacity, 0.0F, 1.0F}, current,
                     placement, blur_shader, texture_size_location, blur_radius_location);
      }
      const float incoming_opacity =
          context_transition.active() ? transition_visual.incoming_opacity : 1.0F;
      draw_content(rendered->compact_content,
                   with_opacity(content_crossfade.compact(), incoming_opacity), current, placement,
                   blur_shader, texture_size_location, blur_radius_location);
      if (rendered->expanded_content) {
        draw_content(*rendered->expanded_content,
                     with_opacity(content_crossfade.expanded(), incoming_opacity), current,
                     placement, blur_shader, texture_size_location, blur_radius_location);
      }
      if (button_hover) {
        const LayoutPlan hover_plan{{}, {*button_hover}, {}};
        if (auto drawn = painter.draw_content(
                hover_plan, RenderOrigin{static_cast<int>(std::lround(placement.x)),
                                         static_cast<int>(std::lround(placement.y))});
            !drawn) {
          std::cerr << drawn.error().message << '\n';
        }
      }
      EndDrawing();
      const bool should_be_visible = mode == IslandMode::expanded || rendered->compact_key;
      if (should_be_visible && !visible) {
        Window::show();
        visible = true;
      } else if (!should_be_visible && visible) {
        Window::hide();
        visible = false;
      }
    } else {
      BeginDrawing();
      ClearBackground(BLANK);
      EndDrawing();
    }
  }

  supervisor.shutdown();
  clear_outgoing();
  if (rendered) {
    unload(*rendered);
  }
  UnloadShader(blur_shader);
  return EXIT_SUCCESS;
}

} // namespace gisland
