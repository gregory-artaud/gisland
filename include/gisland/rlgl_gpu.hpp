#pragma once

#include "gisland/rlgl_font_book.hpp"
#include "gisland/theme.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace gisland {

struct RlglContextState;

enum class RlglGpuError {
  invalid_context,
  invalid_data,
  allocation_failed,
  incomplete_framebuffer,
  gl_error
};

enum class RlglTextureFilter { nearest, linear };

struct RlglContextProbe {
  void *value{};
  bool (*is_current)(void *) noexcept {};

  [[nodiscard]] bool current() const noexcept { return is_current != nullptr && is_current(value); }
};

class RlglSession final {
public:
  [[nodiscard]] static std::expected<RlglSession, RlglGpuError> open(void *extension_loader,
                                                                     int framebuffer_width,
                                                                     int framebuffer_height,
                                                                     RlglContextProbe context);

  RlglSession(const RlglSession &) = delete;
  RlglSession &operator=(const RlglSession &) = delete;
  RlglSession(RlglSession &&other) noexcept;
  RlglSession &operator=(RlglSession &&other) noexcept;
  ~RlglSession();

  [[nodiscard]] bool current() const noexcept;
  [[nodiscard]] bool texture_exists(unsigned int id) const noexcept;
  [[nodiscard]] bool texture_filter_matches(unsigned int id,
                                            RlglTextureFilter filter) const noexcept;
  [[nodiscard]] bool framebuffer_exists(unsigned int id) const noexcept;
  [[nodiscard]] bool program_exists(unsigned int id) const noexcept;
  [[nodiscard]] std::expected<void, RlglGpuError> begin_default_frame(int width, int height,
                                                                      Rgba clear) const noexcept;
  [[nodiscard]] std::expected<void, RlglGpuError> end_default_frame() const noexcept;
  [[nodiscard]] std::expected<std::vector<std::uint8_t>, RlglGpuError>
  read_default_rgba8(int width, int height) const;

private:
  explicit RlglSession(std::shared_ptr<RlglContextState> context) noexcept
      : context_(std::move(context)), active_(true) {}

  std::shared_ptr<RlglContextState> context_;
  bool active_{};

  friend class RlglTexture;
  friend class RlglShader;
  friend class RlglFrame;
};

class RlglTexture final {
public:
  [[nodiscard]] static std::expected<RlglTexture, RlglGpuError>
  rgba8(const RlglSession &session, int width, int height, std::span<const std::uint8_t> pixels,
        RlglTextureFilter filter) noexcept;
  [[nodiscard]] static std::expected<RlglTexture, RlglGpuError>
  font_atlas(const RlglSession &session, const FontAtlasData &atlas,
             RlglTextureFilter filter = RlglTextureFilter::linear);

  RlglTexture(const RlglTexture &) = delete;
  RlglTexture &operator=(const RlglTexture &) = delete;
  RlglTexture(RlglTexture &&other) noexcept;
  RlglTexture &operator=(RlglTexture &&other) noexcept;
  ~RlglTexture();

  [[nodiscard]] unsigned int id() const noexcept { return id_; }
  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  [[nodiscard]] RlglTextureFilter filter() const noexcept { return filter_; }
  [[nodiscard]] std::expected<std::vector<std::uint8_t>, RlglGpuError> read_rgba8() const;

private:
  RlglTexture(std::shared_ptr<RlglContextState> context, unsigned int id, int width, int height,
              RlglTextureFilter filter) noexcept
      : context_(std::move(context)), id_(id), width_(width), height_(height), filter_(filter) {}

  void release() noexcept;

  std::shared_ptr<RlglContextState> context_;
  unsigned int id_{};
  int width_{};
  int height_{};
  RlglTextureFilter filter_{RlglTextureFilter::nearest};
};

class RlglShader final {
public:
  [[nodiscard]] static std::expected<RlglShader, RlglGpuError>
  load(const RlglSession &session, std::string_view vertex_source,
       std::string_view fragment_source);

  RlglShader(const RlglShader &) = delete;
  RlglShader &operator=(const RlglShader &) = delete;
  RlglShader(RlglShader &&other) noexcept;
  RlglShader &operator=(RlglShader &&other) noexcept;
  ~RlglShader();

  [[nodiscard]] unsigned int id() const noexcept { return id_; }

private:
  RlglShader(std::shared_ptr<RlglContextState> context, unsigned int id) noexcept
      : context_(std::move(context)), id_(id) {}

  void release() noexcept;

  std::shared_ptr<RlglContextState> context_;
  unsigned int id_{};
};

class RlglFrame final {
public:
  [[nodiscard]] static std::expected<RlglFrame, RlglGpuError>
  create(const RlglSession &session, int width, int height) noexcept;

  RlglFrame(const RlglFrame &) = delete;
  RlglFrame &operator=(const RlglFrame &) = delete;
  RlglFrame(RlglFrame &&other) noexcept;
  RlglFrame &operator=(RlglFrame &&other) noexcept;
  ~RlglFrame();

  [[nodiscard]] std::expected<void, RlglGpuError> begin(Rgba clear) noexcept;
  [[nodiscard]] std::expected<void, RlglGpuError> end() noexcept;
  [[nodiscard]] std::expected<void, RlglGpuError> present() const noexcept;
  [[nodiscard]] std::expected<std::vector<std::uint8_t>, RlglGpuError> read_rgba8() const;
  [[nodiscard]] unsigned int id() const noexcept { return framebuffer_id_; }
  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }

private:
  RlglFrame(std::shared_ptr<RlglContextState> context, unsigned int framebuffer_id,
            unsigned int texture_id, int width, int height) noexcept
      : context_(std::move(context)), framebuffer_id_(framebuffer_id), texture_id_(texture_id),
        width_(width), height_(height) {}

  void release() noexcept;

  std::shared_ptr<RlglContextState> context_;
  unsigned int framebuffer_id_{};
  unsigned int texture_id_{};
  int width_{};
  int height_{};
  bool active_{};
};

} // namespace gisland
