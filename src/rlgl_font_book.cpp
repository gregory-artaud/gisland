#include "gisland/rlgl_font_book.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <utility>

namespace gisland {
namespace {

constexpr int glyph_padding = 4;
constexpr int atlas_corner_size = 3;
constexpr int text_line_spacing = 2;

struct FontKey {
  std::filesystem::path path;
  int base_size;

  auto operator<=>(const FontKey &) const = default;
};

struct PendingGlyph {
  FontAtlasGlyph glyph;
  int width;
  int height;
  std::vector<std::uint8_t> alpha;
};

[[nodiscard]] RlglFontError error(RlglFontErrorCode code, std::filesystem::path resource,
                                  std::string message) {
  return {code, std::move(resource), std::move(message)};
}

[[nodiscard]] int font_base_size(const TypographyRole &role) {
  return std::max(1, static_cast<int>(std::ceil(role.size)));
}

void add_text_codepoints(std::set<int> &codepoints) {
  constexpr std::array ranges{
      std::pair{0x0020, 0x007E}, std::pair{0x00A0, 0x024F}, std::pair{0x0300, 0x052F},
      std::pair{0x1C80, 0x1C8F}, std::pair{0x1D00, 0x1FFF}, std::pair{0x2000, 0x206F},
      std::pair{0x2C60, 0x2C7F}, std::pair{0x2DE0, 0x2DFF}, std::pair{0xA640, 0xA69F},
      std::pair{0xA720, 0xA7FF}, std::pair{0xAB30, 0xAB6F}, std::pair{0x10780, 0x107BF}};
  for (const auto &[first, last] : ranges) {
    for (int codepoint = first; codepoint <= last; ++codepoint) {
      codepoints.insert(codepoint);
    }
  }
}

template <typename Visitor>
[[nodiscard]] bool visit_utf8(std::string_view text, Visitor &&visitor) {
  for (std::size_t index = 0; index < text.size();) {
    const auto first = static_cast<unsigned char>(text[index]);
    char32_t codepoint{};
    std::size_t length{};
    if (first <= 0x7FU) {
      codepoint = first;
      length = 1;
    } else if (first >= 0xC2U && first <= 0xDFU) {
      codepoint = first & 0x1FU;
      length = 2;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      codepoint = first & 0x0FU;
      length = 3;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      codepoint = first & 0x07U;
      length = 4;
    } else {
      return false;
    }
    if (index + length > text.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto continuation = static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xC0U) != 0x80U) {
        return false;
      }
      codepoint = (codepoint << 6U) | (continuation & 0x3FU);
    }
    if ((length == 2 && codepoint < 0x80U) || (length == 3 && codepoint < 0x800U) ||
        (length == 4 && codepoint < 0x10000U) || (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
        codepoint > 0x10FFFFU || !visitor(codepoint)) {
      return false;
    }
    index += length;
  }
  return true;
}

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::filesystem::path &path) {
  std::ifstream stream{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] const FontAtlasGlyph *find_glyph(const FontAtlasData &atlas, char32_t codepoint) {
  const auto iterator = std::ranges::find(atlas.glyphs, codepoint, &FontAtlasGlyph::codepoint);
  return iterator == atlas.glyphs.end() ? nullptr : &*iterator;
}

} // namespace

std::expected<FontAtlasData, RlglFontError> build_font_atlas(const std::filesystem::path &path,
                                                             int base_size,
                                                             std::span<const int> codepoints) {
  if (base_size <= 0 || codepoints.empty()) {
    return std::unexpected(error(RlglFontErrorCode::font_load_failed, path,
                                 "font size and codepoint list must be non-empty"));
  }
  const auto bytes = read_bytes(path);
  if (bytes.empty()) {
    return std::unexpected(
        error(RlglFontErrorCode::font_load_failed, path, "font resource could not be read"));
  }

  stbtt_fontinfo info{};
  if (stbtt_InitFont(&info, bytes.data(), 0) == 0) {
    return std::unexpected(
        error(RlglFontErrorCode::font_load_failed, path, "font resource could not be parsed"));
  }
  const float scale = stbtt_ScaleForPixelHeight(&info, static_cast<float>(base_size));
  int ascent{};
  int descent{};
  int line_gap{};
  stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
  static_cast<void>(descent);
  static_cast<void>(line_gap);

  std::vector<PendingGlyph> pending;
  pending.reserve(codepoints.size());
  int total_width{};
  for (const int codepoint : codepoints) {
    if (stbtt_FindGlyphIndex(&info, codepoint) <= 0) {
      continue;
    }
    int width{};
    int height{};
    int offset_x{};
    int offset_y{};
    unsigned char *bitmap = stbtt_GetCodepointBitmap(&info, scale, scale, codepoint, &width,
                                                     &height, &offset_x, &offset_y);
    int advance{};
    std::vector<std::uint8_t> alpha;
    int baseline_offset_y = offset_y;
    if (codepoint == 0x20) {
      stbtt_GetCodepointHMetrics(&info, codepoint, &advance, nullptr);
      advance = static_cast<int>(static_cast<float>(advance) * scale);
      width = advance;
      height = base_size;
      alpha.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    } else if (bitmap != nullptr) {
      stbtt_GetCodepointHMetrics(&info, codepoint, &advance, nullptr);
      advance = static_cast<int>(static_cast<float>(advance) * scale);
      alpha.assign(bitmap,
                   bitmap + static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
      baseline_offset_y += static_cast<int>(static_cast<float>(ascent) * scale);
    }
    stbtt_FreeBitmap(bitmap, nullptr);
    pending.push_back({{static_cast<char32_t>(codepoint), offset_x, baseline_offset_y, advance, {}},
                       width,
                       height,
                       std::move(alpha)});
    total_width += width + 2 * glyph_padding;
  }
  if (pending.empty()) {
    return std::unexpected(
        error(RlglFontErrorCode::font_load_failed, path, "font contains no requested glyphs"));
  }

  const int padded_size = base_size + 2 * glyph_padding;
  const float total_area = static_cast<float>(total_width * padded_size) * 1.2F;
  const float minimum_size = std::sqrt(total_area);
  const int image_size =
      static_cast<int>(std::pow(2.0F, std::ceil(std::log(minimum_size) / std::log(2.0F))));
  int atlas_width = image_size;
  int atlas_height =
      total_area < static_cast<float>((image_size * image_size) / 2) ? image_size / 2 : image_size;
  std::vector<std::uint8_t> atlas(static_cast<std::size_t>(atlas_width) *
                                  static_cast<std::size_t>(atlas_height));
  int offset_x = glyph_padding;
  int offset_y = glyph_padding;
  for (auto &entry : pending) {
    if (offset_x >= atlas_width - entry.width - 2 * glyph_padding) {
      offset_x = glyph_padding;
      offset_y += padded_size;
      if (offset_y > atlas_height - base_size - glyph_padding) {
        atlas_height *= 2;
        atlas.resize(static_cast<std::size_t>(atlas_width) *
                     static_cast<std::size_t>(atlas_height));
      }
    }
    for (int y = 0; y < entry.height; ++y) {
      for (int x = 0; x < entry.width; ++x) {
        atlas[static_cast<std::size_t>(offset_y + y) * static_cast<std::size_t>(atlas_width) +
              static_cast<std::size_t>(offset_x + x)] =
            entry.alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(entry.width) +
                        static_cast<std::size_t>(x)];
      }
    }
    entry.glyph.rectangle = {offset_x, offset_y, entry.width, entry.height};
    offset_x += entry.width + 2 * glyph_padding;
  }
  for (int row = 0; row < atlas_corner_size; ++row) {
    const auto end = atlas.end() - static_cast<std::ptrdiff_t>(row * atlas_width);
    std::fill(end - atlas_corner_size, end, std::uint8_t{255});
  }

  std::vector<FontAtlasGlyph> glyphs;
  glyphs.reserve(pending.size());
  for (auto &entry : pending) {
    glyphs.push_back(std::move(entry.glyph));
  }
  return FontAtlasData{base_size,    glyph_padding,    atlas_width,
                       atlas_height, std::move(atlas), std::move(glyphs)};
}

struct RlglFontBook::Impl {
  std::map<std::string, std::filesystem::path, std::less<>> resolved_resources;
  std::map<FontKey, FontAtlasData> atlases;

  [[nodiscard]] const FontAtlasData *find(std::string_view resource,
                                          const TypographyRole &role) const {
    const auto resolved = resolved_resources.find(resource);
    if (resolved == resolved_resources.end()) {
      return nullptr;
    }
    const auto atlas = atlases.find(FontKey{resolved->second, font_base_size(role)});
    return atlas == atlases.end() ? nullptr : &atlas->second;
  }
};

RlglFontBook::RlglFontBook(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
RlglFontBook::RlglFontBook(RlglFontBook &&) noexcept = default;
RlglFontBook &RlglFontBook::operator=(RlglFontBook &&) noexcept = default;
RlglFontBook::~RlglFontBook() = default;

std::expected<RlglFontBook, RlglFontError>
RlglFontBook::load(const Theme &theme, const std::filesystem::path &asset_root) {
  auto impl = std::make_unique<Impl>();
  std::error_code filesystem_error;
  for (const auto &[name, resource] : theme.fonts()) {
    static_cast<void>(name);
    const std::filesystem::path declared{resource};
    const auto resolved =
        (declared.is_absolute() ? declared : asset_root / declared).lexically_normal();
    if (!std::filesystem::exists(resolved, filesystem_error)) {
      return std::unexpected(
          error(RlglFontErrorCode::missing_resource, resolved, "font resource does not exist"));
    }
    if (filesystem_error || !std::filesystem::is_regular_file(resolved, filesystem_error)) {
      return std::unexpected(error(RlglFontErrorCode::invalid_resource, resolved,
                                   "font resource is not a regular readable file"));
    }
    impl->resolved_resources.emplace(resource, resolved);
  }

  const auto &body = theme.typography().at("body");
  std::map<FontKey, std::set<int>> requests;
  const auto request = [&](std::string_view resource, const TypographyRole &role) {
    requests[FontKey{impl->resolved_resources.find(resource)->second, font_base_size(role)}];
  };
  for (const auto &[name, resource] : theme.fonts()) {
    static_cast<void>(name);
    request(resource, body);
  }
  for (const auto &[name, role] : theme.typography()) {
    static_cast<void>(name);
    request(theme.fonts().at(role.font), role);
  }
  std::map<std::filesystem::path, std::set<int>> icons;
  for (const auto &[name, icon] : theme.icons()) {
    static_cast<void>(name);
    const auto &resource = theme.fonts().at(icon.font);
    icons[impl->resolved_resources.at(resource)].insert(static_cast<int>(icon.codepoint));
    request(resource, body);
  }

  for (auto &[key, codepoints] : requests) {
    add_text_codepoints(codepoints);
    if (const auto found = icons.find(key.path); found != icons.end()) {
      codepoints.insert(found->second.begin(), found->second.end());
    }
    const std::vector<int> ordered{codepoints.begin(), codepoints.end()};
    auto atlas = build_font_atlas(key.path, key.base_size, ordered);
    if (!atlas) {
      return std::unexpected(std::move(atlas.error()));
    }
    impl->atlases.emplace(key, std::move(*atlas));
  }
  return RlglFontBook{std::move(impl)};
}

bool RlglFontBook::supports_text(std::string_view font_resource, const TypographyRole &role,
                                 std::string_view text) const {
  return visit_utf8(text, [this, font_resource, &role](char32_t codepoint) {
    return supports_codepoint(font_resource, role, codepoint);
  });
}

bool RlglFontBook::supports_codepoint(std::string_view font_resource, const TypographyRole &role,
                                      char32_t codepoint) const {
  const auto *data = impl_->find(font_resource, role);
  return data != nullptr && find_glyph(*data, codepoint) != nullptr;
}

MeasuredGlyphs RlglFontBook::measure_text(std::string_view font_resource,
                                          const TypographyRole &role, std::string_view text) const {
  const auto *data = impl_->find(font_resource, role);
  if (data == nullptr || text.empty()) {
    return {};
  }
  double width{};
  double maximum_width{};
  double height = role.size;
  const bool valid = visit_utf8(text, [&](char32_t codepoint) {
    if (codepoint == U'\n') {
      maximum_width = std::max(maximum_width, width);
      width = 0.0;
      height += role.size + text_line_spacing;
      return true;
    }
    const auto *glyph = find_glyph(*data, codepoint);
    if (glyph == nullptr) {
      return false;
    }
    width += glyph->advance_x > 0 ? glyph->advance_x : glyph->rectangle.width + glyph->offset_x;
    return true;
  });
  if (!valid) {
    return {};
  }
  maximum_width = std::max(maximum_width, width);
  const double scale = role.size / static_cast<double>(data->base_size);
  return {maximum_width * scale, height * role.line_height};
}

MeasuredGlyphs RlglFontBook::measure_codepoint(std::string_view font_resource,
                                               const TypographyRole &role,
                                               char32_t codepoint) const {
  const auto *data = impl_->find(font_resource, role);
  if (data == nullptr) {
    return {};
  }
  const auto *glyph = find_glyph(*data, codepoint);
  if (glyph == nullptr) {
    return {};
  }
  const double scale = role.size / static_cast<double>(data->base_size);
  const int width =
      glyph->advance_x > 0 ? glyph->advance_x : glyph->rectangle.width + glyph->offset_x;
  return {static_cast<double>(width) * scale, role.size * role.line_height};
}

std::size_t RlglFontBook::loaded_font_count() const noexcept { return impl_->atlases.size(); }

const FontAtlasData *RlglFontBook::atlas(std::string_view font_resource,
                                         const TypographyRole &role) const noexcept {
  return impl_->find(font_resource, role);
}

} // namespace gisland
