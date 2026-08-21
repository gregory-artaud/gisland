#include "fixtures/primitive_gallery.hpp"
#include "gisland/layout.hpp"
#include "gisland/rlgl_gpu.hpp"
#include "gisland/rlgl_painter.hpp"
#include "gisland/rlgl_texture_books.hpp"
#include "gisland/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <EGL/egl.h>
#include <png.h>
#include <rlgl.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <utility>
#include <variant>

namespace {

class EglContext {
public:
  EglContext() {
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY || eglInitialize(display_, nullptr, nullptr) == EGL_FALSE ||
        eglBindAPI(EGL_OPENGL_API) == EGL_FALSE) {
      throw std::runtime_error{"EGL display initialization failed"};
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
      throw std::runtime_error{"EGL config selection failed"};
    }
    constexpr EGLint surface_attributes[]{EGL_WIDTH, 32, EGL_HEIGHT, 32, EGL_NONE};
    surface_ = eglCreatePbufferSurface(display_, config, surface_attributes);
    constexpr EGLint context_attributes[]{EGL_CONTEXT_MAJOR_VERSION,
                                          3,
                                          EGL_CONTEXT_MINOR_VERSION,
                                          3,
                                          EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                          EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                          EGL_NONE};
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, context_attributes);
    if (surface_ == EGL_NO_SURFACE || context_ == EGL_NO_CONTEXT ||
        eglMakeCurrent(display_, surface_, surface_, context_) == EGL_FALSE) {
      throw std::runtime_error{"EGL context creation failed"};
    }
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
    }
  }

  [[nodiscard]] gisland::RlglContextProbe probe() const noexcept {
    return {context_, [](void *expected) noexcept {
              return eglGetCurrentContext() == static_cast<EGLContext>(expected);
            }};
  }

private:
  EGLDisplay display_{EGL_NO_DISPLAY};
  EGLSurface surface_{EGL_NO_SURFACE};
  EGLContext context_{EGL_NO_CONTEXT};
};

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    throw std::runtime_error{"failed to read " + path.string()};
  }
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] gisland::Theme load_theme() {
  const auto path = std::filesystem::path{GISLAND_TEST_ASSET_ROOT} / "themes/default.toml";
  return gisland::parse_theme(read_file(path), path.string()).value();
}

[[nodiscard]] std::vector<std::uint8_t> load_png(const std::filesystem::path &path, int width,
                                                 int height) {
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  if (png_image_begin_read_from_file(&image, path.c_str()) == 0) {
    throw std::runtime_error{image.message};
  }
  image.format = PNG_FORMAT_RGBA;
  if (image.width != static_cast<png_uint_32>(width) ||
      image.height != static_cast<png_uint_32>(height)) {
    png_image_free(&image);
    throw std::runtime_error{"unexpected PNG dimensions"};
  }
  std::vector<std::uint8_t> pixels(PNG_IMAGE_SIZE(image));
  if (png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr) == 0) {
    const std::string message = image.message;
    png_image_free(&image);
    throw std::runtime_error{message};
  }
  png_image_free(&image);
  return pixels;
}

void save_png(const std::filesystem::path &path, int width, int height,
              const std::vector<std::uint8_t> &pixels) {
  std::filesystem::create_directories(path.parent_path());
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  image.width = static_cast<png_uint_32>(width);
  image.height = static_cast<png_uint_32>(height);
  image.format = PNG_FORMAT_RGBA;
  if (png_image_write_to_file(&image, path.c_str(), 0, pixels.data(), 0, nullptr) == 0) {
    throw std::runtime_error{image.message};
  }
}

} // namespace

[[nodiscard]] gisland::Rgba pixel(const std::vector<std::uint8_t> &pixels, int width, int x,
                                  int y) {
  const auto index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                      static_cast<std::size_t>(x)) *
                     4U;
  return {pixels[index], pixels[index + 1U], pixels[index + 2U], pixels[index + 3U]};
}

[[nodiscard]] int colored_pixels(const std::vector<std::uint8_t> &pixels, int width,
                                 gisland::Rect bounds) {
  int count{};
  for (int y = bounds.y; y < bounds.y + bounds.height; ++y) {
    for (int x = bounds.x; x < bounds.x + bounds.width; ++x) {
      if (pixel(pixels, width, x, y).alpha != 0) {
        ++count;
      }
    }
  }
  return count;
}

TEST_CASE("standalone rlgl uploads and releases RGBA textures") {
  EglContext context;
  auto session = gisland::RlglSession::open(reinterpret_cast<void *>(eglGetProcAddress), 32, 32,
                                            context.probe());
  REQUIRE(session.has_value());
  constexpr std::array<std::uint8_t, 16> pixels{255, 0, 0,   255, 0,   255, 0,   128,
                                                0,   0, 255, 64,  255, 255, 255, 0};
  unsigned int texture_id{};
  {
    auto texture =
        gisland::RlglTexture::rgba8(*session, 2, 2, pixels, gisland::RlglTextureFilter::nearest);
    REQUIRE(texture.has_value());
    texture_id = texture->id();
    CHECK(texture->filter() == gisland::RlglTextureFilter::nearest);
    CHECK(session->texture_exists(texture_id));
    CHECK(session->texture_filter_matches(texture_id, gisland::RlglTextureFilter::nearest));
    const auto readback = texture->read_rgba8();
    REQUIRE(readback.has_value());
    CHECK(std::ranges::equal(*readback, pixels));
  }
  CHECK_FALSE(session->texture_exists(texture_id));
}

TEST_CASE("standalone rlgl frame clears and reads canonical top-left RGBA") {
  EglContext context;
  auto session = gisland::RlglSession::open(reinterpret_cast<void *>(eglGetProcAddress), 32, 32,
                                            context.probe());
  REQUIRE(session.has_value());
  auto frame = gisland::RlglFrame::create(*session, 3, 2);
  REQUIRE(frame.has_value());
  REQUIRE(frame->begin({10, 20, 30, 40}).has_value());
  rlBegin(RL_QUADS);
  rlColor4ub(200, 100, 50, 255);
  rlVertex2f(0.0F, 0.0F);
  rlVertex2f(0.0F, 1.0F);
  rlVertex2f(3.0F, 1.0F);
  rlVertex2f(3.0F, 0.0F);
  rlEnd();
  REQUIRE(frame->end().has_value());
  const auto pixels = frame->read_rgba8();
  REQUIRE(pixels.has_value());
  REQUIRE(pixels->size() == 24U);
  for (std::size_t index = 0; index < pixels->size(); index += 4U) {
    const bool top_row = index < 12U;
    CHECK((*pixels)[index] == (top_row ? 200 : 10));
    CHECK((*pixels)[index + 1U] == (top_row ? 100 : 20));
    CHECK((*pixels)[index + 2U] == (top_row ? 50 : 30));
    CHECK((*pixels)[index + 3U] == (top_row ? 255 : 40));
  }
}

TEST_CASE("standalone session renders and reads the native framebuffer") {
  EglContext context;
  auto session = gisland::RlglSession::open(reinterpret_cast<void *>(eglGetProcAddress), 32, 32,
                                            context.probe());
  REQUIRE(session.has_value());
  const gisland::LayoutPlan plan{gisland::RoundedView{{8, 8, 16, 16}, 0, 0, {32, 96, 224, 255}, {}},
                                 {}};
  const gisland::RlglPainter painter{*session};
  REQUIRE(session->begin_default_frame(32, 32, {0, 0, 0, 0}).has_value());
  REQUIRE(painter.draw_surface(plan).has_value());
  REQUIRE(session->end_default_frame().has_value());
  const auto pixels = session->read_default_rgba8(32, 32);
  REQUIRE(pixels.has_value());
  CHECK(pixel(*pixels, 32, 16, 16) == gisland::Rgba{32, 96, 224, 255});
  CHECK(pixel(*pixels, 32, 0, 0) == gisland::Rgba{0, 0, 0, 0});
}

TEST_CASE("standalone frame presents its RGBA target to the native framebuffer") {
  EglContext context;
  auto session = gisland::RlglSession::open(reinterpret_cast<void *>(eglGetProcAddress), 32, 32,
                                            context.probe());
  REQUIRE(session.has_value());
  auto frame = gisland::RlglFrame::create(*session, 32, 32);
  REQUIRE(frame.has_value());
  REQUIRE(frame->begin({10, 20, 30, 40}).has_value());
  CHECK_FALSE(frame->present().has_value());
  REQUIRE(frame->end().has_value());
  REQUIRE(frame->present().has_value());
  const auto pixels = session->read_default_rgba8(32, 32);
  REQUIRE(pixels.has_value());
  CHECK(pixel(*pixels, 32, 16, 16) == gisland::Rgba{10, 20, 30, 40});
}

TEST_CASE("standalone rlgl uploads font atlases and owns custom shaders") {
  EglContext context;
  auto session = gisland::RlglSession::open(reinterpret_cast<void *>(eglGetProcAddress), 32, 32,
                                            context.probe());
  REQUIRE(session.has_value());
  const gisland::FontAtlasData atlas{16, 4, 2, 1, {0, 255}, {}};
  auto texture = gisland::RlglTexture::font_atlas(*session, atlas);
  REQUIRE(texture.has_value());
  CHECK(texture->width() == 2);
  CHECK(texture->height() == 1);
  CHECK(texture->filter() == gisland::RlglTextureFilter::linear);
  CHECK(session->texture_exists(texture->id()));
  CHECK(session->texture_filter_matches(texture->id(), gisland::RlglTextureFilter::linear));

  constexpr std::string_view vertex = R"(#version 330
layout(location = 0) in vec3 vertexPosition;
void main() { gl_Position = vec4(vertexPosition, 1.0); }
)";
  constexpr std::string_view fragment = R"(#version 330
out vec4 finalColor;
void main() { finalColor = vec4(1.0); }
)";
  unsigned int shader_id{};
  {
    auto shader = gisland::RlglShader::load(*session, vertex, fragment);
    REQUIRE(shader.has_value());
    shader_id = shader->id();
    CHECK(session->program_exists(shader_id));
  }
  CHECK_FALSE(session->program_exists(shader_id));
  CHECK_FALSE(gisland::RlglShader::load(*session, "invalid", "invalid").has_value());
}

TEST_CASE("stale standalone resources never delete replacement-context objects") {
  std::optional<gisland::RlglTexture> stale;
  {
    EglContext first_context;
    auto first_session = gisland::RlglSession::open(reinterpret_cast<void *>(eglGetProcAddress), 32,
                                                    32, first_context.probe());
    REQUIRE(first_session.has_value());
    constexpr std::array<std::uint8_t, 4> pixel{1, 2, 3, 4};
    auto texture = gisland::RlglTexture::rgba8(*first_session, 1, 1, pixel,
                                               gisland::RlglTextureFilter::linear);
    REQUIRE(texture.has_value());
    stale.emplace(std::move(*texture));
  }

  EglContext replacement_context;
  auto replacement_session = gisland::RlglSession::open(reinterpret_cast<void *>(eglGetProcAddress),
                                                        32, 32, replacement_context.probe());
  REQUIRE(replacement_session.has_value());
  constexpr std::array<std::uint8_t, 4> replacement_pixel{5, 6, 7, 8};
  auto replacement = gisland::RlglTexture::rgba8(*replacement_session, 1, 1, replacement_pixel,
                                                 gisland::RlglTextureFilter::linear);
  REQUIRE(replacement.has_value());
  stale.reset();
  CHECK(replacement_session->texture_exists(replacement->id()));
  const auto readback = replacement->read_rgba8();
  REQUIRE(readback.has_value());
  CHECK(std::ranges::equal(*readback, replacement_pixel));
}

TEST_CASE("standalone painter draws rounded surface shadow and border") {
  EglContext context;
  auto session = gisland::RlglSession::open(reinterpret_cast<void *>(eglGetProcAddress), 128, 96,
                                            context.probe());
  REQUIRE(session.has_value());
  auto frame = gisland::RlglFrame::create(*session, 128, 96);
  REQUIRE(frame.has_value());
  const gisland::LayoutPlan plan{
      gisland::RoundedView{
          {4, 16, 80, 48},
          12,
          2,
          {16, 24, 32, 255},
          {32, 192, 96, 255},
          {.offset_x = 2, .offset_y = 3, .blur = 2, .spread = 1, .color = {0, 0, 0, 90}}},
      {}};
  const gisland::RlglPainter painter{*session};
  REQUIRE(frame->begin({0, 0, 0, 0}).has_value());
  REQUIRE(painter.draw_surface(plan, {8, 8}).has_value());
  REQUIRE(frame->end().has_value());
  const auto pixels = frame->read_rgba8();
  REQUIRE(pixels.has_value());
  CHECK(pixel(*pixels, 128, 52, 48) == plan.view.surface);
  CHECK(pixel(*pixels, 128, 52, 23) == plan.view.border_color);
  CHECK(pixel(*pixels, 128, 8, 8).alpha == 0);
  CHECK(pixel(*pixels, 128, 93, 50).alpha > 0);
}

TEST_CASE("standalone painter draws clipped geometry commands and rejects text") {
  EglContext context;
  auto session = gisland::RlglSession::open(reinterpret_cast<void *>(eglGetProcAddress), 128, 96,
                                            context.probe());
  REQUIRE(session.has_value());
  auto frame = gisland::RlglFrame::create(*session, 128, 96);
  REQUIRE(frame.has_value());
  const gisland::LayoutPlan plan{
      {},
      {gisland::ProgressDrawCommand{{4, 36, 40, 8},
                                    {14, 36, 30, 8},
                                    {4, 36, 40, 8},
                                    {4, 36, 20, 8},
                                    {64, 80, 96, 255},
                                    {224, 48, 48, 255},
                                    "/progress",
                                    0.5,
                                    std::nullopt},
       gisland::ButtonDecorationDrawCommand{
           {50, 32, 30, 16}, {50, 32, 30, 16}, {48, 80, 224, 255}, true},
       gisland::RingProgressDrawCommand{{16, 52, 32, 32},
                                        {16, 52, 32, 32},
                                        0.25,
                                        4,
                                        {64, 80, 96, 255},
                                        {32, 192, 96, 255},
                                        "/ring",
                                        std::nullopt},
       gisland::IndicatorDrawCommand{
           {88, 36, 7, 7}, {90, 36, 5, 7}, {32, 192, 96, 255}, "Available"}}};
  const gisland::RlglPainter painter{*session};
  REQUIRE(frame->begin({0, 0, 0, 0}).has_value());
  REQUIRE(painter.draw_content(plan, {8, 8}).has_value());
  REQUIRE(frame->end().has_value());
  const auto pixels = frame->read_rgba8();
  REQUIRE(pixels.has_value());
  CHECK(pixel(*pixels, 128, 16, 48).alpha == 0);
  CHECK(pixel(*pixels, 128, 24, 48) == gisland::Rgba{224, 48, 48, 255});
  CHECK(pixel(*pixels, 128, 38, 48) == gisland::Rgba{64, 80, 96, 255});
  CHECK(pixel(*pixels, 128, 73, 50) == gisland::Rgba{48, 80, 224, 255});
  CHECK(pixel(*pixels, 128, 40, 62) == gisland::Rgba{32, 192, 96, 255});
  CHECK(pixel(*pixels, 128, 96, 47).alpha == 0);
  CHECK(pixel(*pixels, 128, 99, 47).alpha > 0);

  const gisland::LayoutPlan unsupported{
      {}, {gisland::TextDrawCommand{{0, 0, 1, 1}, {0, 0, 1, 1}, {}, {}, {}, {}}}};
  CHECK(painter.draw_content(unsupported).error() == gisland::RlglPaintError::unsupported_command);
}

TEST_CASE("standalone painter draws pinned text and icon atlas glyphs") {
  EglContext context;
  auto session = gisland::RlglSession::open(reinterpret_cast<void *>(eglGetProcAddress), 128, 96,
                                            context.probe());
  REQUIRE(session.has_value());
  const auto theme = load_theme();
  auto fonts = gisland::RlglFontBook::load(theme, GISLAND_TEST_ASSET_ROOT);
  REQUIRE(fonts.has_value());
  const auto &body = theme.typography().at("body");
  const auto text_resource = theme.fonts().at(body.font);
  const auto icon_resource = theme.fonts().at(theme.icons().at("calendar").font);
  const gisland::LayoutPlan plan{{},
                                 {gisland::TextDrawCommand{{8, 8, 104, 24},
                                                           {8, 8, 104, 24},
                                                           "Text and icon",
                                                           text_resource,
                                                           body,
                                                           {247, 247, 248, 255}},
                                  gisland::IconDrawCommand{{8, 40, 24, 24},
                                                           {10, 40, 22, 24},
                                                           icon_resource,
                                                           body,
                                                           U'\uF133',
                                                           {255, 214, 10, 255},
                                                           "Calendar"}}};
  auto textures = gisland::RlglFontTextureBook::load(*session, *fonts);
  REQUIRE(textures.has_value());
  REQUIRE(textures->prepare(plan).has_value());
  CHECK(textures->loaded_texture_count() == 2U);

  auto frame = gisland::RlglFrame::create(*session, 128, 96);
  REQUIRE(frame.has_value());
  const gisland::RlglPainter painter{*session, &*textures};
  REQUIRE(frame->begin({0, 0, 0, 0}).has_value());
  REQUIRE(painter.draw_content(plan).has_value());
  REQUIRE(frame->end().has_value());
  const auto pixels = frame->read_rgba8();
  REQUIRE(pixels.has_value());
  CHECK(colored_pixels(*pixels, 128,
                       plan.content.empty()
                           ? gisland::Rect{}
                           : std::get<gisland::TextDrawCommand>(plan.content[0]).bounds) > 0);
  CHECK(pixel(*pixels, 128, 8, 50).alpha == 0);
  CHECK(colored_pixels(*pixels, 128, {10, 40, 22, 24}) > 0);
}

TEST_CASE("standalone painter draws prepared masked images and rich text") {
  EglContext context;
  auto session = gisland::RlglSession::open(reinterpret_cast<void *>(eglGetProcAddress), 128, 96,
                                            context.probe());
  REQUIRE(session.has_value());
  const auto theme = load_theme();
  const auto source = std::make_shared<const std::vector<std::uint8_t>>(
      std::vector<std::uint8_t>{255, 0, 0, 255, 0, 0, 255, 255});
  const std::vector resources{
      gisland::ImageResource{"icon", gisland::ImageFormat::rgba8, 2, 1, source}};
  auto images = gisland::RlglImageBook::load(*session, resources);
  REQUIRE(images.has_value());

  auto pango = gisland::PangoTextBook::load(theme, GISLAND_TEST_ASSET_ROOT);
  REQUIRE(pango.has_value());
  auto rich_textures = gisland::RlglRichTextBook::load(*session, *pango, resources);
  REQUIRE(rich_textures.has_value());
  const gisland::RichText rich{.role = "body", .content = {gisland::RichTextSpan{"Rich text", {}}}};
  const auto composition = pango->compose(rich, 80);
  REQUIRE(composition.has_value());
  const auto &body = theme.typography().at("body");
  const gisland::LayoutPlan plan{{},
                                 {gisland::ImageDrawCommand{{8, 8, 24, 24},
                                                            {8, 8, 24, 24},
                                                            "icon",
                                                            theme.images().at("notification-icon"),
                                                            "Application"},
                                  gisland::RichTextDrawCommand{{40, 8, 80, composition->height},
                                                               {40, 8, 80, composition->height},
                                                               rich,
                                                               *composition,
                                                               theme.fonts().at(body.font),
                                                               body,
                                                               theme.palette().at("foreground"),
                                                               theme.palette().at("accent")}}};
  REQUIRE(images->prepare(plan).has_value());
  REQUIRE(rich_textures->prepare(plan).has_value());
  CHECK(images->loaded_texture_count() == 1U);
  CHECK(rich_textures->loaded_texture_count() == 1U);

  auto frame = gisland::RlglFrame::create(*session, 128, 96);
  REQUIRE(frame.has_value());
  const gisland::RlglPainter painter{*session, nullptr, &*images, &*rich_textures};
  REQUIRE(frame->begin({0, 0, 0, 0}).has_value());
  REQUIRE(painter.draw_content(plan).has_value());
  REQUIRE(frame->end().has_value());
  const auto pixels = frame->read_rgba8();
  REQUIRE(pixels.has_value());
  CHECK(pixel(*pixels, 128, 8, 8).alpha == 0);
  CHECK(pixel(*pixels, 128, 12, 20).red > pixel(*pixels, 128, 12, 20).blue);
  CHECK(pixel(*pixels, 128, 27, 20).blue > pixel(*pixels, 128, 27, 20).red);
  CHECK(colored_pixels(*pixels, 128, {40, 8, 80, composition->height}) > 0);
}

TEST_CASE("standalone painter matches the canonical primitive gallery") {
  constexpr int width = 480;
  constexpr int height = 360;
  EglContext context;
  auto session = gisland::RlglSession::open(reinterpret_cast<void *>(eglGetProcAddress), width,
                                            height, context.probe());
  REQUIRE(session.has_value());
  const auto theme = load_theme();
  auto fonts = gisland::RlglFontBook::load(theme, GISLAND_TEST_ASSET_ROOT);
  REQUIRE(fonts.has_value());
  auto pango = gisland::PangoTextBook::load(theme, GISLAND_TEST_ASSET_ROOT);
  REQUIRE(pango.has_value());
  auto plan = gisland::layout_scene(gisland::test::primitive_gallery(), theme,
                                    gisland::ViewMode::expanded, *fonts, *pango);
  REQUIRE(plan.has_value());
  auto font_textures = gisland::RlglFontTextureBook::load(*session, *fonts);
  REQUIRE(font_textures.has_value());
  REQUIRE(font_textures->prepare(*plan).has_value());
  auto images = gisland::RlglImageBook::load(*session, {});
  REQUIRE(images.has_value());
  REQUIRE(images->prepare(*plan).has_value());
  auto rich_textures = gisland::RlglRichTextBook::load(*session, *pango, {});
  REQUIRE(rich_textures.has_value());
  REQUIRE(rich_textures->prepare(*plan).has_value());

  auto frame = gisland::RlglFrame::create(*session, width, height);
  REQUIRE(frame.has_value());
  const gisland::RenderOrigin origin{(width - plan->view.bounds.width) / 2,
                                     (height - plan->view.bounds.height) / 2};
  const gisland::RlglPainter painter{*session, &*font_textures, &*images, &*rich_textures};
  REQUIRE(frame->begin({0, 0, 0, 0}).has_value());
  REQUIRE(painter.draw_surface(*plan, origin).has_value());
  REQUIRE(painter.draw_content(*plan, origin).has_value());
  REQUIRE(frame->end().has_value());
  const auto actual = frame->read_rgba8();
  REQUIRE(actual.has_value());
  const auto expected = load_png(
      std::filesystem::path{GISLAND_TEST_BASELINE_ROOT} / "all-primitives.png", width, height);
  REQUIRE(actual->size() == expected.size());
  std::size_t divergent_bytes{};
  std::size_t first_divergence = actual->size();
  std::array<std::size_t, 4> divergent_channels{};
  int maximum_delta{};
  int minimum_x = width;
  int minimum_y = height;
  int maximum_x{};
  int maximum_y{};
  for (std::size_t index = 0; index < actual->size(); ++index) {
    if (actual->at(index) != expected[index]) {
      ++divergent_bytes;
      ++divergent_channels[index % 4U];
      maximum_delta = std::max(maximum_delta, std::abs(static_cast<int>(actual->at(index)) -
                                                       static_cast<int>(expected[index])));
      first_divergence = std::min(first_divergence, index);
      const auto pixel_index = index / 4U;
      const int x = static_cast<int>(pixel_index % static_cast<std::size_t>(width));
      const int y = static_cast<int>(pixel_index / static_cast<std::size_t>(width));
      minimum_x = std::min(minimum_x, x);
      minimum_y = std::min(minimum_y, y);
      maximum_x = std::max(maximum_x, x);
      maximum_y = std::max(maximum_y, y);
    }
  }
  INFO("first divergent byte: " << first_divergence);
  INFO("divergent bytes: " << divergent_bytes);
  INFO("first values: "
       << (first_divergence < actual->size() ? static_cast<int>(actual->at(first_divergence)) : -1)
       << " versus "
       << (first_divergence < expected.size() ? static_cast<int>(expected[first_divergence]) : -1));
  INFO("divergent RGBA channels: " << divergent_channels[0] << ',' << divergent_channels[1] << ','
                                   << divergent_channels[2] << ',' << divergent_channels[3]);
  INFO("maximum channel delta: " << maximum_delta);
  INFO("view bounds: " << plan->view.bounds.x << ',' << plan->view.bounds.y << ' '
                       << plan->view.bounds.width << 'x' << plan->view.bounds.height
                       << "; origin: " << origin.x << ',' << origin.y);
  INFO("divergence bounds: " << minimum_x << ',' << minimum_y << " to " << maximum_x << ','
                             << maximum_y);
  if (maximum_delta > 1) {
    save_png(std::filesystem::path{GISLAND_VISUAL_ARTIFACT_ROOT} /
                 "portable-all-primitives-actual.png",
             width, height, *actual);
  }
  CHECK(maximum_delta <= 1);
}
