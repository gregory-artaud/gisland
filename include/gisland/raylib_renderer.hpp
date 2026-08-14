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
  image_load_failed,
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

struct IndicatorAnimationState {
  double elapsed_seconds{};
  bool reduced_motion{};
};

class RaylibPainter;
class RaylibImageBook;
class RaylibRichTextBook;

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

class RaylibImageBook final {
public:
  [[nodiscard]] static std::expected<RaylibImageBook, RendererError>
  load(const std::vector<ImageResource> &resources);

  RaylibImageBook(const RaylibImageBook &) = delete;
  RaylibImageBook &operator=(const RaylibImageBook &) = delete;
  RaylibImageBook(RaylibImageBook &&) noexcept;
  RaylibImageBook &operator=(RaylibImageBook &&) noexcept;
  ~RaylibImageBook();

  [[nodiscard]] std::expected<void, RendererError> prepare(const LayoutPlan &plan);
  [[nodiscard]] std::size_t loaded_texture_count() const noexcept;

private:
  struct Impl;

  explicit RaylibImageBook(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;

  friend class RaylibPainter;
};

class RaylibRichTextBook final {
public:
  [[nodiscard]] static std::expected<RaylibRichTextBook, RendererError>
  load(const PangoTextBook &text, const std::vector<ImageResource> &resources);

  RaylibRichTextBook(const RaylibRichTextBook &) = delete;
  RaylibRichTextBook &operator=(const RaylibRichTextBook &) = delete;
  RaylibRichTextBook(RaylibRichTextBook &&) noexcept;
  RaylibRichTextBook &operator=(RaylibRichTextBook &&) noexcept;
  ~RaylibRichTextBook();

  [[nodiscard]] std::expected<void, RendererError> prepare(const LayoutPlan &plan);
  [[nodiscard]] std::size_t loaded_texture_count() const noexcept;

private:
  struct Impl;

  explicit RaylibRichTextBook(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;

  friend class RaylibPainter;
};

class RaylibPainter final {
public:
  explicit RaylibPainter(const RaylibFontBook &fonts) noexcept : fonts_(fonts) {}
  RaylibPainter(const RaylibFontBook &fonts, const RaylibImageBook &images) noexcept
      : fonts_(fonts), images_(&images) {}
  RaylibPainter(const RaylibFontBook &fonts, const RaylibRichTextBook &rich_text) noexcept
      : fonts_(fonts), rich_text_(&rich_text) {}
  RaylibPainter(const RaylibFontBook &fonts, const RaylibImageBook &images,
                const RaylibRichTextBook &rich_text) noexcept
      : fonts_(fonts), images_(&images), rich_text_(&rich_text) {}

  [[nodiscard]] std::expected<void, RendererError> draw_surface(const LayoutPlan &plan,
                                                                RenderOrigin origin = {}) const;
  [[nodiscard]] std::expected<void, RendererError>
  draw_content(const LayoutPlan &plan, RenderOrigin origin = {},
               IndicatorAnimationState indicator = {}) const;

private:
  const RaylibFontBook &fonts_;
  const RaylibImageBook *images_{};
  const RaylibRichTextBook *rich_text_{};
};

} // namespace gisland
