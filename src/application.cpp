#include "gisland/application.hpp"

#include <raylib.h>

#include <cstdlib>
#include <iostream>
#include <utility>

namespace gisland {
namespace {

class Window final {
public:
  explicit Window(const ApplicationConfig &config) {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(config.width, config.height, config.title.c_str());
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
};

} // namespace

Application::Application(ApplicationConfig config) : config_(std::move(config)) {}

int Application::run() {
  Window window{config_};
  if (!Window::is_ready()) {
    std::cerr << "Failed to initialize the raylib window\n";
    return EXIT_FAILURE;
  }

  SetTargetFPS(config_.target_fps);

  while (!WindowShouldClose()) {
    BeginDrawing();
    ClearBackground(Color{.r = 18, .g = 18, .b = 20, .a = 255});
    EndDrawing();
  }

  return EXIT_SUCCESS;
}

} // namespace gisland
