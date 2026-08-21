#pragma once

#include "gisland/layout.hpp"
#include "gisland/rich_text.hpp"
#include "gisland/rlgl_font_book.hpp"
#include "gisland/rlgl_gpu.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <vector>

namespace gisland {

enum class RlglTextureBookError {
  invalid_resource,
  missing_resource,
  invalid_geometry,
  raster_failed,
  gpu_error
};

struct RlglFontBinding {
  const FontAtlasData *atlas;
  const RlglTexture *texture;
};

class RlglFontTextureBook final {
public:
  [[nodiscard]] static std::expected<RlglFontTextureBook, RlglTextureBookError>
  load(const RlglSession &session, const RlglFontBook &fonts);

  RlglFontTextureBook(const RlglFontTextureBook &) = delete;
  RlglFontTextureBook &operator=(const RlglFontTextureBook &) = delete;
  RlglFontTextureBook(RlglFontTextureBook &&) noexcept;
  RlglFontTextureBook &operator=(RlglFontTextureBook &&) noexcept;
  ~RlglFontTextureBook();

  [[nodiscard]] std::expected<void, RlglTextureBookError> prepare(const LayoutPlan &plan);
  [[nodiscard]] const RlglFontBinding *find(std::string_view resource,
                                            const TypographyRole &role) const noexcept;
  [[nodiscard]] std::size_t loaded_texture_count() const noexcept;

private:
  struct Impl;
  explicit RlglFontTextureBook(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

class RlglImageBook final {
public:
  [[nodiscard]] static std::expected<RlglImageBook, RlglTextureBookError>
  load(const RlglSession &session, const std::vector<ImageResource> &resources);

  RlglImageBook(const RlglImageBook &) = delete;
  RlglImageBook &operator=(const RlglImageBook &) = delete;
  RlglImageBook(RlglImageBook &&) noexcept;
  RlglImageBook &operator=(RlglImageBook &&) noexcept;
  ~RlglImageBook();

  [[nodiscard]] std::expected<void, RlglTextureBookError> prepare(const LayoutPlan &plan);
  [[nodiscard]] const RlglTexture *find(const ImageDrawCommand &command) const noexcept;
  [[nodiscard]] std::size_t loaded_texture_count() const noexcept;

private:
  struct Impl;
  explicit RlglImageBook(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

class RlglRichTextBook final {
public:
  [[nodiscard]] static std::expected<RlglRichTextBook, RlglTextureBookError>
  load(const RlglSession &session, const PangoTextBook &text,
       const std::vector<ImageResource> &resources);

  RlglRichTextBook(const RlglRichTextBook &) = delete;
  RlglRichTextBook &operator=(const RlglRichTextBook &) = delete;
  RlglRichTextBook(RlglRichTextBook &&) noexcept;
  RlglRichTextBook &operator=(RlglRichTextBook &&) noexcept;
  ~RlglRichTextBook();

  [[nodiscard]] std::expected<void, RlglTextureBookError> prepare(const LayoutPlan &plan);
  [[nodiscard]] const RlglTexture *find(const RichTextDrawCommand &command) const noexcept;
  [[nodiscard]] std::size_t loaded_texture_count() const noexcept;

private:
  struct Impl;
  explicit RlglRichTextBook(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

} // namespace gisland
