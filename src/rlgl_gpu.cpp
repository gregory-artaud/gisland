#include "gisland/rlgl_gpu.hpp"

#include <rlgl.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>

bool gisland_rlgl_has_error();
void gisland_rlgl_clear_errors();
bool gisland_rlgl_texture_exists(unsigned int id);
bool gisland_rlgl_texture_filter_matches(unsigned int id, bool linear);
bool gisland_rlgl_framebuffer_exists(unsigned int id);
bool gisland_rlgl_program_exists(unsigned int id);
void gisland_rlgl_read_default_rgba8(int width, int height, unsigned char *pixels);
void gisland_rlgl_blit_to_default(unsigned int framebuffer, int width, int height);
bool gisland_rlgl_marker_matches(unsigned int id, const unsigned char *expected);

namespace gisland {

struct RlglContextState {
  RlglContextProbe probe;
  unsigned int marker_id{};
  std::array<std::uint8_t, 4> marker_color{};
};

namespace {

[[nodiscard]] bool current_context(const std::shared_ptr<RlglContextState> &context) noexcept {
  return context != nullptr && context->marker_id != 0 && context->probe.current() &&
         gisland_rlgl_marker_matches(context->marker_id, context->marker_color.data());
}

[[nodiscard]] bool valid_pixel_count(int width, int height, std::size_t channels,
                                     std::size_t actual) noexcept {
  if (width <= 0 || height <= 0) {
    return false;
  }
  const auto unsigned_width = static_cast<std::size_t>(width);
  const auto unsigned_height = static_cast<std::size_t>(height);
  if (unsigned_width > std::numeric_limits<std::size_t>::max() / unsigned_height ||
      unsigned_width * unsigned_height > std::numeric_limits<std::size_t>::max() / channels) {
    return false;
  }
  return unsigned_width * unsigned_height * channels == actual;
}

void set_filter(unsigned int id, RlglTextureFilter filter) {
  const int value =
      filter == RlglTextureFilter::linear ? RL_TEXTURE_FILTER_LINEAR : RL_TEXTURE_FILTER_NEAREST;
  rlTextureParameters(id, RL_TEXTURE_MIN_FILTER, value);
  rlTextureParameters(id, RL_TEXTURE_MAG_FILTER, value);
}

[[nodiscard]] std::expected<std::vector<std::uint8_t>, RlglGpuError>
read_texture(const std::shared_ptr<RlglContextState> &context, unsigned int id, int width,
             int height) {
  if (!current_context(context)) {
    return std::unexpected(RlglGpuError::invalid_context);
  }
  gisland_rlgl_clear_errors();
  void *data = rlReadTexturePixels(id, width, height, RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
  if (data == nullptr || gisland_rlgl_has_error()) {
    std::free(data);
    return std::unexpected(RlglGpuError::gl_error);
  }
  const std::size_t size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  std::vector<std::uint8_t> result(bytes, bytes + size);
  std::free(data);
  return result;
}

} // namespace

std::expected<RlglSession, RlglGpuError> RlglSession::open(void *extension_loader,
                                                           int framebuffer_width,
                                                           int framebuffer_height,
                                                           RlglContextProbe context) {
  if (extension_loader == nullptr || framebuffer_width <= 0 || framebuffer_height <= 0 ||
      !context.current()) {
    return std::unexpected(RlglGpuError::invalid_context);
  }
  rlLoadExtensions(extension_loader);
  gisland_rlgl_clear_errors();
  rlglInit(framebuffer_width, framebuffer_height);
  if (rlGetVersion() != RL_OPENGL_33 || rlGetFramebufferWidth() != framebuffer_width ||
      rlGetFramebufferHeight() != framebuffer_height || gisland_rlgl_has_error()) {
    if (context.current()) {
      rlglClose();
    }
    return std::unexpected(RlglGpuError::gl_error);
  }
  auto state = std::make_shared<RlglContextState>();
  state->probe = context;
  std::uint64_t seed = reinterpret_cast<std::uintptr_t>(state.get());
  seed ^= seed >> 30U;
  seed *= 0xBF58476D1CE4E5B9ULL;
  seed ^= seed >> 27U;
  state->marker_color = {static_cast<std::uint8_t>((seed >> 0U) | 1U),
                         static_cast<std::uint8_t>((seed >> 8U) | 1U),
                         static_cast<std::uint8_t>((seed >> 16U) | 1U), 255};
  state->marker_id =
      rlLoadTexture(state->marker_color.data(), 1, 1, RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
  if (state->marker_id == 0 || gisland_rlgl_has_error()) {
    rlglClose();
    return std::unexpected(RlglGpuError::allocation_failed);
  }
  return RlglSession{std::move(state)};
}

RlglSession::RlglSession(RlglSession &&other) noexcept
    : context_(std::move(other.context_)), active_(std::exchange(other.active_, false)) {}

RlglSession &RlglSession::operator=(RlglSession &&other) noexcept {
  if (this != &other) {
    if (active_ && current_context(context_)) {
      rlUnloadTexture(context_->marker_id);
      context_->marker_id = 0;
      rlglClose();
    } else if (context_ != nullptr) {
      context_->marker_id = 0;
    }
    context_ = std::move(other.context_);
    active_ = std::exchange(other.active_, false);
  }
  return *this;
}

RlglSession::~RlglSession() {
  if (active_ && current_context(context_)) {
    rlUnloadTexture(context_->marker_id);
    context_->marker_id = 0;
    rlglClose();
  } else if (active_ && context_ != nullptr) {
    context_->marker_id = 0;
  }
}

bool RlglSession::current() const noexcept { return active_ && current_context(context_); }

bool RlglSession::texture_exists(unsigned int id) const noexcept {
  return current() && id != 0 && gisland_rlgl_texture_exists(id);
}

bool RlglSession::texture_filter_matches(unsigned int id, RlglTextureFilter filter) const noexcept {
  return texture_exists(id) &&
         gisland_rlgl_texture_filter_matches(id, filter == RlglTextureFilter::linear);
}

bool RlglSession::framebuffer_exists(unsigned int id) const noexcept {
  return current() && id != 0 && gisland_rlgl_framebuffer_exists(id);
}

bool RlglSession::program_exists(unsigned int id) const noexcept {
  return current() && id != 0 && gisland_rlgl_program_exists(id);
}

std::expected<void, RlglGpuError> RlglSession::begin_default_frame(int width, int height,
                                                                   Rgba clear) const noexcept {
  if (!current()) {
    return std::unexpected(RlglGpuError::invalid_context);
  }
  if (width <= 0 || height <= 0) {
    return std::unexpected(RlglGpuError::invalid_data);
  }
  gisland_rlgl_clear_errors();
  rlDisableFramebuffer();
  rlSetFramebufferWidth(width);
  rlSetFramebufferHeight(height);
  rlViewport(0, 0, width, height);
  rlClearColor(clear.red, clear.green, clear.blue, clear.alpha);
  rlClearScreenBuffers();
  rlMatrixMode(RL_PROJECTION);
  rlLoadIdentity();
  rlOrtho(0.0, static_cast<double>(width), static_cast<double>(height), 0.0, -1.0, 1.0);
  rlMatrixMode(RL_MODELVIEW);
  rlLoadIdentity();
  return gisland_rlgl_has_error() ? std::unexpected(RlglGpuError::gl_error)
                                  : std::expected<void, RlglGpuError>{};
}

std::expected<void, RlglGpuError> RlglSession::end_default_frame() const noexcept {
  if (!current()) {
    return std::unexpected(RlglGpuError::invalid_context);
  }
  rlDrawRenderBatchActive();
  return gisland_rlgl_has_error() ? std::unexpected(RlglGpuError::gl_error)
                                  : std::expected<void, RlglGpuError>{};
}

std::expected<std::vector<std::uint8_t>, RlglGpuError>
RlglSession::read_default_rgba8(int width, int height) const {
  if (!current()) {
    return std::unexpected(RlglGpuError::invalid_context);
  }
  if (width <= 0 || height <= 0 ||
      static_cast<std::size_t>(width) >
          std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(height) / 4U) {
    return std::unexpected(RlglGpuError::invalid_data);
  }
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height) * 4U);
  gisland_rlgl_clear_errors();
  gisland_rlgl_read_default_rgba8(width, height, pixels.data());
  if (gisland_rlgl_has_error()) {
    return std::unexpected(RlglGpuError::gl_error);
  }
  const std::size_t row_size = static_cast<std::size_t>(width) * 4U;
  for (int row = 0; row < height / 2; ++row) {
    const auto top =
        pixels.begin() + static_cast<std::ptrdiff_t>(row) * static_cast<std::ptrdiff_t>(row_size);
    const auto bottom = pixels.begin() + static_cast<std::ptrdiff_t>(height - row - 1) *
                                             static_cast<std::ptrdiff_t>(row_size);
    std::swap_ranges(top, top + static_cast<std::ptrdiff_t>(row_size), bottom);
  }
  return pixels;
}

std::expected<RlglTexture, RlglGpuError> RlglTexture::rgba8(const RlglSession &session, int width,
                                                            int height,
                                                            std::span<const std::uint8_t> pixels,
                                                            RlglTextureFilter filter) noexcept {
  if (!session.current()) {
    return std::unexpected(RlglGpuError::invalid_context);
  }
  if (!valid_pixel_count(width, height, 4U, pixels.size())) {
    return std::unexpected(RlglGpuError::invalid_data);
  }
  gisland_rlgl_clear_errors();
  const unsigned int id =
      rlLoadTexture(pixels.data(), width, height, RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
  if (id == 0) {
    return std::unexpected(RlglGpuError::allocation_failed);
  }
  set_filter(id, filter);
  if (gisland_rlgl_has_error()) {
    rlUnloadTexture(id);
    return std::unexpected(RlglGpuError::gl_error);
  }
  return RlglTexture{session.context_, id, width, height, filter};
}

std::expected<RlglTexture, RlglGpuError> RlglTexture::font_atlas(const RlglSession &session,
                                                                 const FontAtlasData &atlas,
                                                                 RlglTextureFilter filter) {
  if (!session.current()) {
    return std::unexpected(RlglGpuError::invalid_context);
  }
  if (!valid_pixel_count(atlas.width, atlas.height, 1U, atlas.alpha.size())) {
    return std::unexpected(RlglGpuError::invalid_data);
  }
  std::vector<std::uint8_t> gray_alpha(atlas.alpha.size() * 2U);
  for (std::size_t index = 0; index < atlas.alpha.size(); ++index) {
    gray_alpha[index * 2U] = 255;
    gray_alpha[index * 2U + 1U] = atlas.alpha[index];
  }
  gisland_rlgl_clear_errors();
  const unsigned int id = rlLoadTexture(gray_alpha.data(), atlas.width, atlas.height,
                                        RL_PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA, 1);
  if (id == 0) {
    return std::unexpected(RlglGpuError::allocation_failed);
  }
  set_filter(id, filter);
  if (gisland_rlgl_has_error()) {
    rlUnloadTexture(id);
    return std::unexpected(RlglGpuError::gl_error);
  }
  return RlglTexture{session.context_, id, atlas.width, atlas.height, filter};
}

RlglTexture::RlglTexture(RlglTexture &&other) noexcept
    : context_(other.context_), id_(std::exchange(other.id_, 0)), width_(other.width_),
      height_(other.height_), filter_(other.filter_) {}

RlglTexture &RlglTexture::operator=(RlglTexture &&other) noexcept {
  if (this != &other) {
    release();
    context_ = other.context_;
    id_ = std::exchange(other.id_, 0);
    width_ = other.width_;
    height_ = other.height_;
    filter_ = other.filter_;
  }
  return *this;
}

RlglTexture::~RlglTexture() { release(); }

void RlglTexture::release() noexcept {
  if (id_ != 0 && current_context(context_)) {
    rlUnloadTexture(id_);
  }
  id_ = 0;
}

std::expected<std::vector<std::uint8_t>, RlglGpuError> RlglTexture::read_rgba8() const {
  return read_texture(context_, id_, width_, height_);
}

std::expected<RlglShader, RlglGpuError> RlglShader::load(const RlglSession &session,
                                                         std::string_view vertex_source,
                                                         std::string_view fragment_source) {
  if (!session.current()) {
    return std::unexpected(RlglGpuError::invalid_context);
  }
  if (vertex_source.empty() || fragment_source.empty()) {
    return std::unexpected(RlglGpuError::invalid_data);
  }
  const std::string vertex{vertex_source};
  const std::string fragment{fragment_source};
  gisland_rlgl_clear_errors();
  const unsigned int id = rlLoadShaderProgram(vertex.c_str(), fragment.c_str());
  if (id == 0 || id == rlGetShaderIdDefault()) {
    return std::unexpected(RlglGpuError::allocation_failed);
  }
  if (gisland_rlgl_has_error()) {
    rlUnloadShaderProgram(id);
    return std::unexpected(RlglGpuError::gl_error);
  }
  return RlglShader{session.context_, id};
}

RlglShader::RlglShader(RlglShader &&other) noexcept
    : context_(other.context_), id_(std::exchange(other.id_, 0)) {}

RlglShader &RlglShader::operator=(RlglShader &&other) noexcept {
  if (this != &other) {
    release();
    context_ = other.context_;
    id_ = std::exchange(other.id_, 0);
  }
  return *this;
}

RlglShader::~RlglShader() { release(); }

void RlglShader::release() noexcept {
  if (id_ != 0 && current_context(context_) && gisland_rlgl_program_exists(id_)) {
    rlUnloadShaderProgram(id_);
  }
  id_ = 0;
}

std::expected<RlglFrame, RlglGpuError> RlglFrame::create(const RlglSession &session, int width,
                                                         int height) noexcept {
  if (!session.current()) {
    return std::unexpected(RlglGpuError::invalid_context);
  }
  if (width <= 0 || height <= 0) {
    return std::unexpected(RlglGpuError::invalid_data);
  }
  gisland_rlgl_clear_errors();
  const unsigned int framebuffer = rlLoadFramebuffer();
  const unsigned int texture =
      rlLoadTexture(nullptr, width, height, RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
  if (framebuffer == 0 || texture == 0) {
    if (texture != 0) {
      rlUnloadTexture(texture);
    }
    if (framebuffer != 0) {
      rlUnloadFramebuffer(framebuffer);
    }
    return std::unexpected(RlglGpuError::allocation_failed);
  }
  set_filter(texture, RlglTextureFilter::nearest);
  rlFramebufferAttach(framebuffer, texture, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D,
                      0);
  if (!rlFramebufferComplete(framebuffer)) {
    rlUnloadTexture(texture);
    rlUnloadFramebuffer(framebuffer);
    return std::unexpected(RlglGpuError::incomplete_framebuffer);
  }
  if (gisland_rlgl_has_error()) {
    rlUnloadTexture(texture);
    rlUnloadFramebuffer(framebuffer);
    return std::unexpected(RlglGpuError::gl_error);
  }
  return RlglFrame{session.context_, framebuffer, texture, width, height};
}

RlglFrame::RlglFrame(RlglFrame &&other) noexcept
    : context_(other.context_), framebuffer_id_(std::exchange(other.framebuffer_id_, 0)),
      texture_id_(std::exchange(other.texture_id_, 0)), width_(other.width_),
      height_(other.height_), active_(std::exchange(other.active_, false)) {}

RlglFrame &RlglFrame::operator=(RlglFrame &&other) noexcept {
  if (this != &other) {
    release();
    context_ = other.context_;
    framebuffer_id_ = std::exchange(other.framebuffer_id_, 0);
    texture_id_ = std::exchange(other.texture_id_, 0);
    width_ = other.width_;
    height_ = other.height_;
    active_ = std::exchange(other.active_, false);
  }
  return *this;
}

RlglFrame::~RlglFrame() { release(); }

void RlglFrame::release() noexcept {
  if (current_context(context_)) {
    if (active_) {
      rlDrawRenderBatchActive();
      rlDisableFramebuffer();
    }
    if (framebuffer_id_ != 0) {
      rlUnloadFramebuffer(framebuffer_id_);
    }
    if (texture_id_ != 0) {
      rlUnloadTexture(texture_id_);
    }
  }
  framebuffer_id_ = 0;
  texture_id_ = 0;
  active_ = false;
}

std::expected<void, RlglGpuError> RlglFrame::begin(Rgba clear) noexcept {
  if (!current_context(context_) || framebuffer_id_ == 0) {
    return std::unexpected(RlglGpuError::invalid_context);
  }
  gisland_rlgl_clear_errors();
  rlEnableFramebuffer(framebuffer_id_);
  rlSetFramebufferWidth(width_);
  rlSetFramebufferHeight(height_);
  rlViewport(0, 0, width_, height_);
  rlClearColor(clear.red, clear.green, clear.blue, clear.alpha);
  rlClearScreenBuffers();
  rlMatrixMode(RL_PROJECTION);
  rlLoadIdentity();
  rlOrtho(0.0, static_cast<double>(width_), static_cast<double>(height_), 0.0, -1.0, 1.0);
  rlMatrixMode(RL_MODELVIEW);
  rlLoadIdentity();
  active_ = true;
  if (gisland_rlgl_has_error()) {
    active_ = false;
    rlDisableFramebuffer();
    return std::unexpected(RlglGpuError::gl_error);
  }
  return {};
}

std::expected<void, RlglGpuError> RlglFrame::end() noexcept {
  if (!active_ || !current_context(context_)) {
    return std::unexpected(RlglGpuError::invalid_context);
  }
  rlDrawRenderBatchActive();
  const bool failed = gisland_rlgl_has_error();
  rlDisableFramebuffer();
  active_ = false;
  return failed ? std::unexpected(RlglGpuError::gl_error) : std::expected<void, RlglGpuError>{};
}

std::expected<void, RlglGpuError> RlglFrame::present() const noexcept {
  if (active_ || !current_context(context_) || framebuffer_id_ == 0) {
    return std::unexpected(RlglGpuError::invalid_context);
  }
  gisland_rlgl_clear_errors();
  gisland_rlgl_blit_to_default(framebuffer_id_, width_, height_);
  return gisland_rlgl_has_error() ? std::unexpected(RlglGpuError::gl_error)
                                  : std::expected<void, RlglGpuError>{};
}

std::expected<std::vector<std::uint8_t>, RlglGpuError> RlglFrame::read_rgba8() const {
  auto pixels = read_texture(context_, texture_id_, width_, height_);
  if (!pixels) {
    return pixels;
  }
  const std::size_t row_size = static_cast<std::size_t>(width_) * 4U;
  for (int row = 0; row < height_ / 2; ++row) {
    const auto top =
        pixels->begin() + static_cast<std::ptrdiff_t>(row) * static_cast<std::ptrdiff_t>(row_size);
    const auto bottom = pixels->begin() + static_cast<std::ptrdiff_t>(height_ - row - 1) *
                                              static_cast<std::ptrdiff_t>(row_size);
    std::swap_ranges(top, top + static_cast<std::ptrdiff_t>(row_size), bottom);
  }
  return pixels;
}

} // namespace gisland
