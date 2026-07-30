#include "gisland/application.hpp"
#include "gisland/island.hpp"
#include "gisland/x11_shape.hpp"

#include <raylib.h>
#include <rlgl.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace gisland {
namespace {

constexpr int content_font_size = 18;
constexpr int content_padding = 12;

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
  explicit Window(const ApplicationConfig &config) {
    const auto canvas = island_canvas_size();
    SetConfigFlags(FLAG_WINDOW_TRANSPARENT | FLAG_WINDOW_UNDECORATED | FLAG_WINDOW_TOPMOST |
                   FLAG_WINDOW_ALWAYS_RUN | FLAG_WINDOW_HIDDEN);
    InitWindow(static_cast<int>(canvas.width), static_cast<int>(canvas.height),
               config.title.c_str());
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

  void apply_shape(const IslandGeometry &geometry, const IslandPlacement &placement) {
    shape_.apply(GetWindowHandle(), geometry, placement);
  }

private:
  RoundedWindowShape shape_;
};

void center_window_at_top(const IslandCanvasSize &canvas) {
  const int monitor = GetCurrentMonitor();
  const Vector2 origin = GetMonitorPosition(monitor);
  const int width = static_cast<int>(std::lround(canvas.width));
  const int x = static_cast<int>(origin.x) + ((GetMonitorWidth(monitor) - width) / 2);
  const int y = static_cast<int>(origin.y) + 8;

  SetWindowPosition(x, y);
}

void draw_island(const IslandGeometry &geometry, const IslandPlacement &placement) {
  const float half_short_side = std::min(geometry.width, geometry.height) / 2.0F;
  const float roundness = std::clamp(geometry.radius / half_short_side, 0.0F, 1.0F);
  const Rectangle bounds{
      .x = placement.x,
      .y = placement.y,
      .width = geometry.width,
      .height = geometry.height,
  };

  DrawRectangleRounded(bounds, roundness, 24, BLACK);
}

RenderTexture2D make_content_texture(const char *label) {
  const int text_width = MeasureText(label, content_font_size);
  RenderTexture2D texture = LoadRenderTexture(text_width + (2 * content_padding),
                                              content_font_size + (2 * content_padding));
  BeginTextureMode(texture);
  ClearBackground(BLANK);
  rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ZERO, RL_ONE, RL_ZERO, RL_FUNC_ADD, RL_FUNC_ADD);
  BeginBlendMode(BLEND_CUSTOM_SEPARATE);
  DrawText(label, content_padding, content_padding, content_font_size, RAYWHITE);
  EndBlendMode();
  EndTextureMode();
  SetTextureFilter(texture.texture, TEXTURE_FILTER_BILINEAR);
  return texture;
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

} // namespace

Application::Application(ApplicationConfig config) : config_(std::move(config)) {}

int Application::run() {
  Window window{config_};
  if (!Window::is_ready()) {
    std::cerr << "Failed to initialize the raylib window\n";
    return EXIT_FAILURE;
  }

  SetTargetFPS(config_.target_fps);

  const RenderTexture2D compact_content = make_content_texture("gisland");
  const RenderTexture2D expanded_content = make_content_texture("expanded");
  const Shader blur_shader = LoadShaderFromMemory(nullptr, content_blur_shader);
  const int texture_size_location = GetShaderLocation(blur_shader, "textureSize");
  const int blur_radius_location = GetShaderLocation(blur_shader, "blurRadius");

  const IslandCanvasSize canvas = island_canvas_size();
  const IslandGeometry compact = geometry_for(IslandMode::compact);
  const IslandGeometry expanded = geometry_for(IslandMode::expanded);
  HoverController hover;
  IslandMode mode = hover.mode();
  SpringProgress spring;
  ContentCrossfade content_crossfade;
  IslandGeometry current = compact;
  IslandPlacement placement = place_at_top_center(current, canvas);

  center_window_at_top(canvas);
  window.apply_shape(current, placement);

  BeginDrawing();
  ClearBackground(BLANK);
  draw_island(current, placement);
  draw_content(compact_content, content_crossfade.compact(), current, placement, blur_shader,
               texture_size_location, blur_radius_location);
  draw_content(expanded_content, content_crossfade.expanded(), current, placement, blur_shader,
               texture_size_location, blur_radius_location);
  EndDrawing();
  Window::show();

  while (!WindowShouldClose()) {
    const float delta_seconds = GetFrameTime();
    hover.update(IsCursorOnScreen(), delta_seconds);
    if (IsKeyPressed(KEY_ESCAPE)) {
      hover.collapse();
    }

    const IslandMode next_mode = hover.mode();
    if (next_mode != mode) {
      mode = next_mode;
      spring.set_target(mode == IslandMode::expanded ? 1.0F : 0.0F);
      content_crossfade.set_mode(mode);
    }

    spring.update(delta_seconds);
    content_crossfade.update(delta_seconds);
    current = interpolate(compact, expanded, spring.value());
    placement = place_at_top_center(current, canvas);

    BeginDrawing();
    ClearBackground(BLANK);
    draw_island(current, placement);
    draw_content(compact_content, content_crossfade.compact(), current, placement, blur_shader,
                 texture_size_location, blur_radius_location);
    draw_content(expanded_content, content_crossfade.expanded(), current, placement, blur_shader,
                 texture_size_location, blur_radius_location);
    EndDrawing();
    window.apply_shape(current, placement);
  }

  UnloadShader(blur_shader);
  UnloadRenderTexture(expanded_content);
  UnloadRenderTexture(compact_content);

  return EXIT_SUCCESS;
}

} // namespace gisland
