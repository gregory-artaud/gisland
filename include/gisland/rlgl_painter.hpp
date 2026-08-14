#pragma once

#include "gisland/layout.hpp"
#include "gisland/rlgl_gpu.hpp"

#include <expected>

namespace gisland {

class RlglFontTextureBook;
class RlglImageBook;
class RlglRichTextBook;

enum class RlglPaintError { invalid_context, invalid_geometry, unsupported_command, gpu_error };

class RlglPainter final {
public:
  explicit RlglPainter(const RlglSession &session, const RlglFontTextureBook *fonts = nullptr,
                       const RlglImageBook *images = nullptr,
                       const RlglRichTextBook *rich_text = nullptr) noexcept
      : session_(session), fonts_(fonts), images_(images), rich_text_(rich_text) {}

  [[nodiscard]] std::expected<void, RlglPaintError> draw_surface(const LayoutPlan &plan,
                                                                 RenderOrigin origin = {}) const;
  [[nodiscard]] std::expected<void, RlglPaintError> draw_content(const LayoutPlan &plan,
                                                                 RenderOrigin origin = {}) const;

private:
  const RlglSession &session_;
  const RlglFontTextureBook *fonts_;
  const RlglImageBook *images_;
  const RlglRichTextBook *rich_text_;
};

} // namespace gisland
