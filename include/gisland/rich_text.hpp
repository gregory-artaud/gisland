#pragma once

#include "gisland/scene.hpp"
#include "gisland/theme.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace gisland {

struct RichTextRect {
  int x;
  int y;
  int width;
  int height;

  bool operator==(const RichTextRect &) const = default;
};

struct RichLinkRectangle {
  RichTextRect bounds;
  std::string action_id;
  std::string accessible_label;
};

struct RichInlineImageRectangle {
  RichTextRect bounds;
  std::string resource_id;
  std::string role;
  std::string accessible_label;
};

struct RichTextComposition {
  int assigned_width;
  int natural_width;
  int minimum_width;
  int height;
  int line_count;
  int unknown_glyphs;
  std::vector<RichLinkRectangle> links;
  std::vector<RichInlineImageRectangle> images;
};

enum class RichTextErrorCode {
  invalid_width,
  invalid_utf8,
  unknown_role,
  unknown_image_role,
  missing_font,
  invalid_font,
  unsupported_glyph,
  invalid_surface,
  missing_resource
};

struct RichTextError {
  RichTextErrorCode code;
  std::string path;
  std::string message;
};

struct RichTextSurface {
  int width;
  int height;
  std::vector<std::uint8_t> pixels;
  RichTextComposition composition;
};

[[nodiscard]] std::expected<std::vector<std::uint8_t>, RichTextError>
cairo_argb32_to_rgba(std::span<const std::uint8_t> source, int width, int height, int stride);

class RichTextMetrics {
public:
  virtual ~RichTextMetrics() = default;

  [[nodiscard]] virtual std::expected<RichTextComposition, RichTextError>
  compose(const RichText &rich_text, int assigned_width) const = 0;
};

class PangoTextBook final : public RichTextMetrics {
public:
  PangoTextBook(PangoTextBook &&) noexcept;
  PangoTextBook &operator=(PangoTextBook &&) noexcept;
  ~PangoTextBook() override;

  PangoTextBook(const PangoTextBook &) = delete;
  PangoTextBook &operator=(const PangoTextBook &) = delete;

  [[nodiscard]] static std::expected<PangoTextBook, RichTextError>
  load(const Theme &theme, const std::filesystem::path &asset_root);

  [[nodiscard]] std::expected<RichTextComposition, RichTextError>
  compose(const RichText &rich_text, int assigned_width) const override;

  [[nodiscard]] std::expected<RichTextSurface, RichTextError>
  rasterize(const RichText &rich_text, int assigned_width,
            const std::vector<ImageResource> &resources) const;

private:
  struct Impl;

  explicit PangoTextBook(std::unique_ptr<Impl> impl) noexcept;

  [[nodiscard]] std::expected<RichTextComposition, RichTextError>
  compose_with_cairo(const RichText &rich_text, int assigned_width, void *cairo_context) const;

  std::unique_ptr<Impl> impl_;
};

} // namespace gisland
