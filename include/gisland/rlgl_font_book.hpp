#pragma once

#include "gisland/layout.hpp"
#include "gisland/theme.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gisland {

enum class RlglFontErrorCode {
  missing_resource,
  invalid_resource,
  font_load_failed,
  font_not_loaded
};

struct RlglFontError {
  RlglFontErrorCode code;
  std::filesystem::path resource;
  std::string message;
};

struct FontAtlasGlyph {
  char32_t codepoint;
  int offset_x;
  int offset_y;
  int advance_x;
  Rect rectangle;
};

struct FontAtlasData {
  int base_size;
  int padding;
  int width;
  int height;
  std::vector<std::uint8_t> alpha;
  std::vector<FontAtlasGlyph> glyphs;
};

[[nodiscard]] std::expected<FontAtlasData, RlglFontError>
build_font_atlas(const std::filesystem::path &path, int base_size, std::span<const int> codepoints);

class RlglFontBook final : public GlyphMetrics {
public:
  [[nodiscard]] static std::expected<RlglFontBook, RlglFontError>
  load(const Theme &theme, const std::filesystem::path &asset_root);

  RlglFontBook(const RlglFontBook &) = delete;
  RlglFontBook &operator=(const RlglFontBook &) = delete;
  RlglFontBook(RlglFontBook &&) noexcept;
  RlglFontBook &operator=(RlglFontBook &&) noexcept;
  ~RlglFontBook() override;

  [[nodiscard]] bool supports_text(std::string_view font_resource, const TypographyRole &role,
                                   std::string_view text) const override;
  [[nodiscard]] bool supports_codepoint(std::string_view font_resource, const TypographyRole &role,
                                        char32_t codepoint) const override;
  [[nodiscard]] MeasuredGlyphs measure_text(std::string_view font_resource,
                                            const TypographyRole &role,
                                            std::string_view text) const override;
  [[nodiscard]] MeasuredGlyphs measure_codepoint(std::string_view font_resource,
                                                 const TypographyRole &role,
                                                 char32_t codepoint) const override;
  [[nodiscard]] std::size_t loaded_font_count() const noexcept;
  [[nodiscard]] const FontAtlasData *atlas(std::string_view font_resource,
                                           const TypographyRole &role) const noexcept;

private:
  struct Impl;
  explicit RlglFontBook(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

} // namespace gisland
