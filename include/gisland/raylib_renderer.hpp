#pragma once

#include "gisland/layout.hpp"
#include "gisland/theme.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace gisland {

enum class RendererErrorCode {
  window_not_ready,
  missing_resource,
  invalid_resource,
  font_load_failed,
  font_not_loaded,
  invalid_geometry
};

struct RendererError {
  RendererErrorCode code;
  std::filesystem::path resource;
  std::string message;
};

struct RenderOrigin {
  int x{};
  int y{};
};

class RaylibPainter;

class RaylibFontBook final : public GlyphMetrics {
public:
  [[nodiscard]] static std::expected<RaylibFontBook, RendererError>
  load(const Theme &theme, const std::filesystem::path &asset_root);

  RaylibFontBook(const RaylibFontBook &) = delete;
  RaylibFontBook &operator=(const RaylibFontBook &) = delete;
  RaylibFontBook(RaylibFontBook &&) noexcept;
  RaylibFontBook &operator=(RaylibFontBook &&) noexcept;
  ~RaylibFontBook() override;

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

private:
  struct Impl;

  explicit RaylibFontBook(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;

  friend class RaylibPainter;
};

class RaylibPainter final {
public:
  explicit RaylibPainter(const RaylibFontBook &fonts) noexcept : fonts_(fonts) {}

  [[nodiscard]] std::expected<void, RendererError> draw_surface(const LayoutPlan &plan,
                                                                RenderOrigin origin = {}) const;
  [[nodiscard]] std::expected<void, RendererError> draw_content(const LayoutPlan &plan,
                                                                RenderOrigin origin = {}) const;

private:
  const RaylibFontBook &fonts_;
};

} // namespace gisland
