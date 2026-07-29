#include "gisland/application.hpp"
#include "gisland/island.hpp"
#include "gisland/x11_shape.hpp"

#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace gisland {
namespace {

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

void draw_island(const IslandGeometry &geometry, const IslandPlacement &placement,
                 IslandMode mode) {
  const float half_short_side = std::min(geometry.width, geometry.height) / 2.0F;
  const float roundness = std::clamp(geometry.radius / half_short_side, 0.0F, 1.0F);
  const Rectangle bounds{
      .x = placement.x,
      .y = placement.y,
      .width = geometry.width,
      .height = geometry.height,
  };

  DrawRectangleRounded(bounds, roundness, 24, BLACK);

  const char *label = mode == IslandMode::expanded ? "expanded" : "gisland";
  constexpr int font_size = 18;
  const int text_width = MeasureText(label, font_size);
  const int text_x =
      static_cast<int>(placement.x + ((geometry.width - static_cast<float>(text_width)) / 2.0F));
  const int text_y =
      static_cast<int>(placement.y + ((geometry.height - static_cast<float>(font_size)) / 2.0F));
  DrawText(label, text_x, text_y, font_size, RAYWHITE);
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

  const IslandCanvasSize canvas = island_canvas_size();
  const IslandGeometry compact = geometry_for(IslandMode::compact);
  const IslandGeometry expanded = geometry_for(IslandMode::expanded);
  HoverController hover;
  IslandMode mode = hover.mode();
  SpringProgress spring;
  IslandGeometry current = compact;
  IslandPlacement placement = place_at_top_center(current, canvas);

  center_window_at_top(canvas);
  window.apply_shape(current, placement);

  BeginDrawing();
  ClearBackground(BLANK);
  draw_island(current, placement, mode);
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
    }

    spring.update(delta_seconds);
    current = interpolate(compact, expanded, spring.value());
    placement = place_at_top_center(current, canvas);

    BeginDrawing();
    ClearBackground(BLANK);
    draw_island(current, placement, mode);
    EndDrawing();
    window.apply_shape(current, placement);
  }

  return EXIT_SUCCESS;
}

} // namespace gisland
