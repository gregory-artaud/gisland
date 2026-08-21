#include "gisland/layout.hpp"
#include "gisland/raylib_renderer.hpp"
#include "gisland/theme.hpp"
#include "primitive_gallery.hpp"

#include <raylib.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr int width = 480;
constexpr int height = 360;

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream stream{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::vector<std::uint8_t> render_gallery() {
  const auto theme_path = std::filesystem::path{GISLAND_TEST_ASSET_ROOT} / "themes/default.toml";
  auto theme = gisland::parse_theme(read_file(theme_path), theme_path.string());
  if (!theme) {
    return {};
  }
  auto fonts = gisland::RaylibFontBook::load(*theme, GISLAND_TEST_ASSET_ROOT);
  auto pango = gisland::PangoTextBook::load(*theme, GISLAND_TEST_ASSET_ROOT);
  if (!fonts || !pango) {
    return {};
  }
  auto plan = gisland::layout_scene(gisland::test::primitive_gallery(), *theme,
                                    gisland::ViewMode::expanded, *fonts, *pango);
  auto images = gisland::RaylibImageBook::load({});
  auto rich_textures = gisland::RaylibRichTextBook::load(*pango, {});
  if (!plan || !images || !rich_textures || !images->prepare(*plan) ||
      !rich_textures->prepare(*plan)) {
    return {};
  }
  const gisland::RenderOrigin origin{(width - plan->view.bounds.width) / 2,
                                     (height - plan->view.bounds.height) / 2};
  const gisland::RaylibPainter painter{*fonts, *images, *rich_textures};
  RenderTexture2D target = LoadRenderTexture(width, height);
  if (!IsRenderTextureValid(target)) {
    return {};
  }
  BeginTextureMode(target);
  ClearBackground(BLANK);
  const auto surface = painter.draw_surface(*plan, origin);
  const auto content =
      surface ? painter.draw_content(*plan, origin)
              : std::expected<void, gisland::RendererError>{std::unexpected(surface.error())};
  EndTextureMode();
  if (!content) {
    UnloadRenderTexture(target);
    return {};
  }
  Image image = LoadImageFromTexture(target.texture);
  UnloadRenderTexture(target);
  ImageFlipVertical(&image);
  ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
  const auto *bytes = static_cast<const std::uint8_t *>(image.data);
  std::vector<std::uint8_t> pixels(bytes, bytes + static_cast<std::size_t>(width * height * 4));
  UnloadImage(image);
  return pixels;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: gisland_raylib_gallery_reference OUTPUT_RGBA\n";
    return 2;
  }
  SetTraceLogLevel(LOG_ERROR);
  SetConfigFlags(FLAG_WINDOW_HIDDEN);
  InitWindow(width, height, "gisland raylib gallery reference");
  if (!IsWindowReady()) {
    std::cerr << "raylib window initialization failed\n";
    return 1;
  }
  const auto pixels = render_gallery();
  CloseWindow();
  if (pixels.size() != static_cast<std::size_t>(width * height * 4)) {
    std::cerr << "raylib gallery rendering failed\n";
    return 1;
  }
  std::ofstream output{argv[1], std::ios::binary};
  output.write(reinterpret_cast<const char *>(pixels.data()),
               static_cast<std::streamsize>(pixels.size()));
  if (!output) {
    std::cerr << "failed to write raylib RGBA output\n";
    return 1;
  }
  return 0;
}
