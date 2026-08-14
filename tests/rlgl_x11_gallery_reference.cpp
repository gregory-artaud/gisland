#include "gisland/layout.hpp"
#include "gisland/rlgl_painter.hpp"
#include "gisland/rlgl_texture_books.hpp"
#include "gisland/theme.hpp"
#include "primitive_gallery.hpp"

#include <EGL/egl.h>

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

class EglContext final {
public:
  EglContext() {
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY || eglInitialize(display_, nullptr, nullptr) == EGL_FALSE ||
        eglBindAPI(EGL_OPENGL_API) == EGL_FALSE) {
      return;
    }
    constexpr EGLint config_attributes[]{EGL_SURFACE_TYPE,
                                         EGL_PBUFFER_BIT,
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
    EGLint count{};
    if (eglChooseConfig(display_, config_attributes, &config, 1, &count) == EGL_FALSE ||
        count == 0) {
      return;
    }
    constexpr EGLint surface_attributes[]{EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    surface_ = eglCreatePbufferSurface(display_, config, surface_attributes);
    constexpr EGLint context_attributes[]{EGL_CONTEXT_MAJOR_VERSION,
                                          3,
                                          EGL_CONTEXT_MINOR_VERSION,
                                          3,
                                          EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                          EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                          EGL_NONE};
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, context_attributes);
    ready_ = surface_ != EGL_NO_SURFACE && context_ != EGL_NO_CONTEXT &&
             eglMakeCurrent(display_, surface_, surface_, context_) == EGL_TRUE;
  }

  EglContext(const EglContext &) = delete;
  EglContext &operator=(const EglContext &) = delete;

  ~EglContext() {
    if (display_ != EGL_NO_DISPLAY) {
      eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(display_, surface_);
      }
      if (context_ != EGL_NO_CONTEXT) {
        eglDestroyContext(display_, context_);
      }
      eglTerminate(display_);
    }
  }

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] gisland::RlglContextProbe probe() const noexcept {
    return {context_, [](void *expected) noexcept {
              return eglGetCurrentContext() == static_cast<EGLContext>(expected);
            }};
  }

private:
  EGLDisplay display_{EGL_NO_DISPLAY};
  EGLSurface surface_{EGL_NO_SURFACE};
  EGLContext context_{EGL_NO_CONTEXT};
  bool ready_{};
};

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    return {};
  }
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: gisland_rlgl_x11_gallery_reference OUTPUT_RGBA\n";
    return 2;
  }
  EglContext context;
  if (!context.ready()) {
    std::cerr << "X11 EGL context initialization failed\n";
    return 1;
  }
  auto session = gisland::RlglSession::open(reinterpret_cast<void *>(eglGetProcAddress), width,
                                            height, context.probe());
  const auto theme_path = std::filesystem::path{GISLAND_TEST_ASSET_ROOT} / "themes/default.toml";
  auto theme = gisland::parse_theme(read_file(theme_path), theme_path.string());
  if (!session || !theme) {
    std::cerr << "portable renderer initialization failed\n";
    return 1;
  }
  auto fonts = gisland::RlglFontBook::load(*theme, GISLAND_TEST_ASSET_ROOT);
  auto pango = gisland::PangoTextBook::load(*theme, GISLAND_TEST_ASSET_ROOT);
  if (!fonts || !pango) {
    std::cerr << "portable renderer font initialization failed\n";
    return 1;
  }
  auto plan = gisland::layout_scene(gisland::test::primitive_gallery(), *theme,
                                    gisland::ViewMode::expanded, *fonts, *pango);
  auto font_textures = gisland::RlglFontTextureBook::load(*session, *fonts);
  auto images = gisland::RlglImageBook::load(*session, {});
  auto rich_textures = gisland::RlglRichTextBook::load(*session, *pango, {});
  if (!plan || !font_textures || !images || !rich_textures || !font_textures->prepare(*plan) ||
      !images->prepare(*plan) || !rich_textures->prepare(*plan)) {
    std::cerr << "portable gallery preparation failed\n";
    return 1;
  }
  const gisland::RenderOrigin origin{(width - plan->view.bounds.width) / 2,
                                     (height - plan->view.bounds.height) / 2};
  const gisland::RlglPainter painter{*session, &*font_textures, &*images, &*rich_textures};
  auto frame = gisland::RlglFrame::create(*session, width, height);
  auto begun = frame ? frame->begin({0, 0, 0, 0})
                     : std::expected<void, gisland::RlglGpuError>{
                           std::unexpected(gisland::RlglGpuError::allocation_failed)};
  auto surface = begun ? painter.draw_surface(*plan, origin)
                       : std::expected<void, gisland::RlglPaintError>{
                             std::unexpected(gisland::RlglPaintError::gpu_error)};
  auto content =
      surface ? painter.draw_content(*plan, origin)
              : std::expected<void, gisland::RlglPaintError>{std::unexpected(surface.error())};
  auto ended = frame && content ? frame->end()
                                : std::expected<void, gisland::RlglGpuError>{
                                      std::unexpected(gisland::RlglGpuError::gl_error)};
  auto pixels = content && ended ? frame->read_rgba8()
                                 : std::expected<std::vector<std::uint8_t>, gisland::RlglGpuError>{
                                       std::unexpected(gisland::RlglGpuError::gl_error)};
  if (!pixels) {
    std::cerr << "portable gallery rendering failed\n";
    return 1;
  }
  std::ofstream output{argv[1], std::ios::binary};
  output.write(reinterpret_cast<const char *>(pixels->data()),
               static_cast<std::streamsize>(pixels->size()));
  if (!output) {
    std::cerr << "failed to write canonical RGBA output\n";
    return 1;
  }
  return 0;
}
