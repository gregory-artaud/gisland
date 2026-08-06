#include "gisland/rich_text.hpp"

#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace gisland {
namespace {

struct GObjectUnref {
  template <typename Value> void operator()(Value *value) const noexcept {
    if (value != nullptr) {
      g_object_unref(value);
    }
  }
};

struct AttributeListUnref {
  void operator()(PangoAttrList *attributes) const noexcept {
    if (attributes != nullptr) {
      pango_attr_list_unref(attributes);
    }
  }
};

struct LayoutIterFree {
  void operator()(PangoLayoutIter *iterator) const noexcept {
    if (iterator != nullptr) {
      pango_layout_iter_free(iterator);
    }
  }
};

using LayoutPtr = std::unique_ptr<PangoLayout, GObjectUnref>;
using AttributeListPtr = std::unique_ptr<PangoAttrList, AttributeListUnref>;
using LayoutIterPtr = std::unique_ptr<PangoLayoutIter, LayoutIterFree>;

struct FreeTypeLibrary {
  FT_Library value{};

  ~FreeTypeLibrary() {
    if (value != nullptr) {
      FT_Done_FreeType(value);
    }
  }
};

struct ByteRange {
  std::size_t start;
  std::size_t end;
};

struct LinkRange {
  ByteRange bytes;
  std::string action_id;
  std::string accessible_label;
};

struct ImageRange {
  std::size_t index;
  std::string resource_id;
  std::string role;
  std::string accessible_label;
  int width;
  int height;
};

struct PreparedText {
  std::string text;
  AttributeListPtr attributes{pango_attr_list_new()};
  std::vector<LinkRange> links;
  std::vector<ImageRange> images;
};

[[nodiscard]] RichTextError error(RichTextErrorCode code, std::string path, std::string message) {
  return RichTextError{code, std::move(path), std::move(message)};
}

void insert_attribute(PangoAttrList *attributes, PangoAttribute *attribute, std::size_t start,
                      std::size_t end) {
  attribute->start_index = static_cast<guint>(start);
  attribute->end_index = static_cast<guint>(end);
  pango_attr_list_insert(attributes, attribute);
}

void insert_base_attribute(PangoAttrList *attributes, PangoAttribute *attribute, std::size_t end) {
  attribute->start_index = 0;
  attribute->end_index = static_cast<guint>(end);
  pango_attr_list_insert_before(attributes, attribute);
}

void add_emphasis(PangoAttrList *attributes, const std::vector<TextEmphasis> &emphasis,
                  std::size_t start, std::size_t end) {
  for (const auto value : emphasis) {
    switch (value) {
    case TextEmphasis::bold:
      insert_attribute(attributes, pango_attr_weight_new(PANGO_WEIGHT_BOLD), start, end);
      break;
    case TextEmphasis::italic:
      insert_attribute(attributes, pango_attr_style_new(PANGO_STYLE_ITALIC), start, end);
      break;
    case TextEmphasis::underline:
      insert_attribute(attributes, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE), start, end);
      break;
    }
  }
}

[[nodiscard]] int pixel_floor(int value) {
  return static_cast<int>(std::floor(static_cast<double>(value) / PANGO_SCALE));
}

[[nodiscard]] int pixel_ceil(int value) {
  return static_cast<int>(std::ceil(static_cast<double>(value) / PANGO_SCALE));
}

[[nodiscard]] std::uint16_t color_component(std::uint8_t value) {
  return static_cast<std::uint16_t>(value) * 257U;
}

[[nodiscard]] bool inside_mask(int x, int y, int width, int height, ImageShape shape,
                               double radius) {
  if (shape == ImageShape::rectangle) {
    return true;
  }
  const double pixel_x = static_cast<double>(x) + 0.5;
  const double pixel_y = static_cast<double>(y) + 0.5;
  if (shape == ImageShape::circle) {
    const double center_x = static_cast<double>(width) / 2.0;
    const double center_y = static_cast<double>(height) / 2.0;
    const double delta_x = pixel_x - center_x;
    const double delta_y = pixel_y - center_y;
    const double circle_radius = static_cast<double>(std::min(width, height)) / 2.0;
    return (delta_x * delta_x) + (delta_y * delta_y) <= circle_radius * circle_radius;
  }
  const double bounded_radius =
      std::clamp(radius, 0.0, static_cast<double>(std::min(width, height)) / 2.0);
  const double closest_x =
      std::clamp(pixel_x, bounded_radius, static_cast<double>(width) - bounded_radius);
  const double closest_y =
      std::clamp(pixel_y, bounded_radius, static_cast<double>(height) - bounded_radius);
  const double delta_x = pixel_x - closest_x;
  const double delta_y = pixel_y - closest_y;
  return (delta_x * delta_x) + (delta_y * delta_y) <= bounded_radius * bounded_radius;
}

[[nodiscard]] std::vector<std::uint8_t>
prepare_image_pixels(const ImageResource &resource, const ImageRole &role, int width, int height) {
  std::vector<std::uint8_t> output(static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height) * 4U);
  const double scale_x = static_cast<double>(width) / resource.width;
  const double scale_y = static_cast<double>(height) / resource.height;
  const double scale =
      role.fit == ImageFit::cover ? std::max(scale_x, scale_y) : std::min(scale_x, scale_y);
  const double rendered_width = static_cast<double>(resource.width) * scale;
  const double rendered_height = static_cast<double>(resource.height) * scale;
  const double offset_x = (static_cast<double>(width) - rendered_width) / 2.0;
  const double offset_y = (static_cast<double>(height) - rendered_height) / 2.0;
  const auto source_channel = [&](int x, int y, std::size_t channel) {
    const std::size_t index =
        (static_cast<std::size_t>(y) * resource.width + static_cast<std::size_t>(x)) * 4U + channel;
    return static_cast<double>(resource.pixels->at(index));
  };
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const double center_x = static_cast<double>(x) + 0.5;
      const double center_y = static_cast<double>(y) + 0.5;
      if (center_x < offset_x || center_x >= offset_x + rendered_width || center_y < offset_y ||
          center_y >= offset_y + rendered_height ||
          !inside_mask(x, y, width, height, role.shape, role.radius)) {
        continue;
      }
      const double source_x = std::clamp((center_x - offset_x) / scale - 0.5, 0.0,
                                         static_cast<double>(resource.width - 1U));
      const double source_y = std::clamp((center_y - offset_y) / scale - 0.5, 0.0,
                                         static_cast<double>(resource.height - 1U));
      const int x0 = static_cast<int>(std::floor(source_x));
      const int y0 = static_cast<int>(std::floor(source_y));
      const int x1 = std::min(x0 + 1, static_cast<int>(resource.width) - 1);
      const int y1 = std::min(y0 + 1, static_cast<int>(resource.height) - 1);
      const double fraction_x = source_x - x0;
      const double fraction_y = source_y - y0;
      const std::size_t output_index =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) *
          4U;
      for (std::size_t channel = 0; channel < 4; ++channel) {
        const double top =
            std::lerp(source_channel(x0, y0, channel), source_channel(x1, y0, channel), fraction_x);
        const double bottom =
            std::lerp(source_channel(x0, y1, channel), source_channel(x1, y1, channel), fraction_x);
        output[output_index + channel] =
            static_cast<std::uint8_t>(std::lround(std::lerp(top, bottom, fraction_y)));
      }
    }
  }
  return output;
}

void source_over(std::span<std::uint8_t, 4> destination, std::span<const std::uint8_t, 4> source) {
  const unsigned source_alpha = source[3];
  const unsigned destination_alpha = destination[3];
  const unsigned inverse_source = 255U - source_alpha;
  const unsigned output_alpha = source_alpha + ((destination_alpha * inverse_source + 127U) / 255U);
  if (output_alpha == 0) {
    std::ranges::fill(destination, std::uint8_t{0});
    return;
  }
  for (std::size_t channel = 0; channel < 3; ++channel) {
    const unsigned premultiplied =
        static_cast<unsigned>(source[channel]) * source_alpha +
        ((static_cast<unsigned>(destination[channel]) * destination_alpha * inverse_source + 127U) /
         255U);
    destination[channel] =
        static_cast<std::uint8_t>((premultiplied + output_alpha / 2U) / output_alpha);
  }
  destination[3] = static_cast<std::uint8_t>(output_alpha);
}

} // namespace

std::expected<std::vector<std::uint8_t>, RichTextError>
cairo_argb32_to_rgba(std::span<const std::uint8_t> source, int width, int height, int stride) {
  if (width < 0 || height < 0 || stride < width * 4 ||
      source.size() < static_cast<std::size_t>(stride) * static_cast<std::size_t>(height)) {
    return std::unexpected(error(RichTextErrorCode::invalid_surface, "/surface",
                                 "Cairo surface dimensions are invalid"));
  }
  std::vector<std::uint8_t> output(static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height) * 4U);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t source_index = static_cast<std::size_t>(y * stride + x * 4);
      const std::size_t output_index =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) *
          4U;
      const std::uint8_t alpha = std::endian::native == std::endian::little
                                     ? source[source_index + 3U]
                                     : source[source_index];
      const auto native_channel = [&](std::size_t rgba_channel) {
        const std::size_t offset =
            std::endian::native == std::endian::little ? 2U - rgba_channel : 1U + rgba_channel;
        return source[source_index + offset];
      };
      for (std::size_t channel = 0; channel < 3; ++channel) {
        output[output_index + channel] =
            alpha == 0
                ? 0
                : static_cast<std::uint8_t>(std::min(
                      255U, (static_cast<unsigned>(native_channel(channel)) * 255U + alpha / 2U) /
                                alpha));
      }
      output[output_index + 3U] = alpha;
    }
  }
  return output;
}

struct PangoTextBook::Impl {
  Theme::Typography typography;
  Theme::Palette palette;
  Theme::ImageRoles image_roles;
  Theme::FontResources fonts;
  std::map<std::string, std::string> font_families;
  FcConfig *font_config{};
  PangoFontMap *font_map{};
  PangoContext *context{};

  ~Impl() {
    if (context != nullptr) {
      g_object_unref(context);
    }
    if (font_map != nullptr) {
      pango_fc_font_map_shutdown(PANGO_FC_FONT_MAP(font_map));
      g_object_unref(font_map);
    }
    if (font_config != nullptr) {
      FcConfigAppFontClear(font_config);
      FcConfigDestroy(font_config);
    }
  }
};

PangoTextBook::PangoTextBook(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

PangoTextBook::PangoTextBook(PangoTextBook &&) noexcept = default;

PangoTextBook &PangoTextBook::operator=(PangoTextBook &&) noexcept = default;

PangoTextBook::~PangoTextBook() = default;

std::expected<PangoTextBook, RichTextError>
PangoTextBook::load(const Theme &theme, const std::filesystem::path &asset_root) {
  auto impl = std::make_unique<Impl>();
  impl->typography = theme.typography();
  impl->palette = theme.palette();
  impl->image_roles = theme.images();
  impl->fonts = theme.fonts();
  impl->font_config = FcConfigCreate();
  if (impl->font_config == nullptr) {
    return std::unexpected(
        error(RichTextErrorCode::invalid_font, "/fonts", "Fontconfig initialization failed"));
  }

  FreeTypeLibrary free_type;
  if (FT_Init_FreeType(&free_type.value) != 0) {
    return std::unexpected(
        error(RichTextErrorCode::invalid_font, "/fonts", "FreeType initialization failed"));
  }

  for (const auto &[name, resource] : theme.fonts()) {
    const std::filesystem::path declared{resource};
    const auto resolved =
        (declared.is_absolute() ? declared : asset_root / declared).lexically_normal();
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(resolved, filesystem_error) || filesystem_error) {
      return std::unexpected(error(RichTextErrorCode::missing_font, "/fonts/" + name,
                                   "font resource is not a readable regular file"));
    }

    const auto *path = reinterpret_cast<const FcChar8 *>(resolved.c_str());
    if (FcConfigAppFontAddFile(impl->font_config, path) == FcFalse) {
      return std::unexpected(error(RichTextErrorCode::invalid_font, "/fonts/" + name,
                                   "Fontconfig rejected the font resource"));
    }

    FT_Face face = nullptr;
    if (FT_New_Face(free_type.value, resolved.c_str(), 0, &face) != 0 || face == nullptr) {
      return std::unexpected(error(RichTextErrorCode::invalid_font, "/fonts/" + name,
                                   "FreeType could not read the font resource"));
    }
    const bool valid_face = face->num_faces == 1 && face->family_name != nullptr;
    const std::string family = valid_face ? face->family_name : "";
    FT_Done_Face(face);
    if (!valid_face) {
      return std::unexpected(error(RichTextErrorCode::invalid_font, "/fonts/" + name,
                                   "font resource must contain one named font face"));
    }
    impl->font_families.emplace(resource, family);
  }

  impl->font_map = pango_cairo_font_map_new();
  if (impl->font_map == nullptr || !PANGO_IS_FC_FONT_MAP(impl->font_map)) {
    return std::unexpected(error(RichTextErrorCode::invalid_font, "/fonts",
                                 "PangoCairo font map initialization failed"));
  }
  pango_fc_font_map_set_config(PANGO_FC_FONT_MAP(impl->font_map), impl->font_config);
  pango_cairo_font_map_set_resolution(PANGO_CAIRO_FONT_MAP(impl->font_map), 96.0);

  impl->context = pango_font_map_create_context(impl->font_map);
  if (impl->context == nullptr) {
    return std::unexpected(
        error(RichTextErrorCode::invalid_font, "/fonts", "Pango context initialization failed"));
  }

  return PangoTextBook{std::move(impl)};
}

std::expected<RichTextComposition, RichTextError> PangoTextBook::compose(const RichText &rich_text,
                                                                         int assigned_width) const {
  return compose_with_cairo(rich_text, assigned_width, nullptr);
}

std::expected<RichTextComposition, RichTextError>
PangoTextBook::compose_with_cairo(const RichText &rich_text, int assigned_width,
                                  void *cairo_context) const {
  if (assigned_width <= 0) {
    return std::unexpected(
        error(RichTextErrorCode::invalid_width, "/width", "assigned width must be positive"));
  }
  const auto role_iterator = impl_->typography.find(rich_text.role);
  if (role_iterator == impl_->typography.end()) {
    return std::unexpected(
        error(RichTextErrorCode::unknown_role, "/role", "unknown rich typography role"));
  }
  const auto &role = role_iterator->second;

  PreparedText prepared;
  for (std::size_t index = 0; index < rich_text.content.size(); ++index) {
    const auto item_path = "/content/" + std::to_string(index);
    const auto result = std::visit(
        [&](const auto &item) -> std::expected<void, RichTextError> {
          using Item = std::decay_t<decltype(item)>;
          if constexpr (std::is_same_v<Item, RichTextSpan> || std::is_same_v<Item, RichLinkSpan>) {
            if (!g_utf8_validate(item.value.data(), static_cast<gssize>(item.value.size()),
                                 nullptr)) {
              return std::unexpected(error(RichTextErrorCode::invalid_utf8, item_path + "/value",
                                           "rich text must be valid UTF-8"));
            }
            const auto start = prepared.text.size();
            prepared.text += item.value;
            const auto end = prepared.text.size();
            add_emphasis(prepared.attributes.get(), item.emphasis, start, end);
            if constexpr (std::is_same_v<Item, RichLinkSpan>) {
              const auto accent = impl_->palette.find("accent");
              if (accent == impl_->palette.end()) {
                return std::unexpected(
                    error(RichTextErrorCode::unknown_role, item_path, "theme has no accent color"));
              }
              insert_attribute(prepared.attributes.get(),
                               pango_attr_underline_new(PANGO_UNDERLINE_SINGLE), start, end);
              insert_attribute(prepared.attributes.get(),
                               pango_attr_foreground_new(color_component(accent->second.red),
                                                         color_component(accent->second.green),
                                                         color_component(accent->second.blue)),
                               start, end);
              prepared.links.push_back(
                  LinkRange{{start, end}, item.action_id, item.accessible_label});
            }
          } else {
            const auto image_role = impl_->image_roles.find(item.role);
            if (image_role == impl_->image_roles.end()) {
              return std::unexpected(error(RichTextErrorCode::unknown_image_role,
                                           item_path + "/role", "unknown inline image role"));
            }
            const int width = static_cast<int>(std::lround(image_role->second.width));
            const int height = static_cast<int>(std::lround(image_role->second.height));
            const auto start = prepared.text.size();
            prepared.text += "\xEF\xBF\xBC";
            const PangoRectangle shape{0, -height * PANGO_SCALE, width * PANGO_SCALE,
                                       height * PANGO_SCALE};
            insert_attribute(prepared.attributes.get(), pango_attr_shape_new(&shape, &shape), start,
                             prepared.text.size());
            prepared.images.push_back(ImageRange{start, item.resource_id, item.role,
                                                 item.accessible_label, width, height});
          }
          return {};
        },
        rich_text.content[index]);
    if (!result.has_value()) {
      return std::unexpected(result.error());
    }
  }

  auto font_description =
      std::unique_ptr<PangoFontDescription, decltype(&pango_font_description_free)>{
          pango_font_description_new(), pango_font_description_free};
  const auto font = impl_->fonts.find(role.font);
  if (font == impl_->fonts.end()) {
    return std::unexpected(
        error(RichTextErrorCode::invalid_font, "/role", "typography font was not declared"));
  }
  const auto theme_font = impl_->font_families.find(font->second);
  if (theme_font == impl_->font_families.end()) {
    return std::unexpected(
        error(RichTextErrorCode::invalid_font, "/role", "typography font was not registered"));
  }
  pango_font_description_set_family(font_description.get(), theme_font->second.c_str());
  pango_font_description_set_absolute_size(font_description.get(), role.size * PANGO_SCALE);
  pango_font_description_set_weight(font_description.get(), static_cast<PangoWeight>(role.weight));

  const auto foreground = impl_->palette.find(role.color);
  if (foreground == impl_->palette.end()) {
    return std::unexpected(
        error(RichTextErrorCode::unknown_role, "/role", "typography color was not registered"));
  }
  insert_base_attribute(prepared.attributes.get(),
                        pango_attr_foreground_new(color_component(foreground->second.red),
                                                  color_component(foreground->second.green),
                                                  color_component(foreground->second.blue)),
                        prepared.text.size());
  insert_base_attribute(prepared.attributes.get(), pango_attr_line_height_new(role.line_height),
                        prepared.text.size());
  insert_base_attribute(prepared.attributes.get(), pango_attr_fallback_new(FALSE),
                        prepared.text.size());

  const auto make_layout = [&](int width, PangoWrapMode wrap) {
    LayoutPtr layout{pango_layout_new(impl_->context)};
    pango_layout_set_font_description(layout.get(), font_description.get());
    pango_layout_set_text(layout.get(), prepared.text.c_str(),
                          static_cast<int>(prepared.text.size()));
    pango_layout_set_attributes(layout.get(), prepared.attributes.get());
    pango_layout_set_wrap(layout.get(), wrap);
    pango_layout_set_width(layout.get(), width);
    return layout;
  };

  auto natural_layout = make_layout(-1, PANGO_WRAP_WORD_CHAR);
  int natural_width = 0;
  int unused_height = 0;
  pango_layout_get_pixel_size(natural_layout.get(), &natural_width, &unused_height);

  auto minimum_layout = make_layout(PANGO_SCALE, PANGO_WRAP_CHAR);
  int minimum_width = 0;
  pango_layout_get_pixel_size(minimum_layout.get(), &minimum_width, &unused_height);

  auto layout = make_layout(assigned_width * PANGO_SCALE, PANGO_WRAP_WORD_CHAR);
  const int unknown_glyphs = pango_layout_get_unknown_glyphs_count(layout.get());
  if (unknown_glyphs != 0) {
    return std::unexpected(error(RichTextErrorCode::unsupported_glyph, "/content",
                                 "registered theme font does not support all rich text glyphs"));
  }
  int content_width = 0;
  int height = 0;
  pango_layout_get_pixel_size(layout.get(), &content_width, &height);
  static_cast<void>(content_width);
  if (cairo_context != nullptr) {
    auto *context = static_cast<cairo_t *>(cairo_context);
    pango_cairo_update_layout(context, layout.get());
    cairo_move_to(context, 0.0, 0.0);
    pango_cairo_show_layout(context, layout.get());
  }

  RichTextComposition composition{
      .assigned_width = assigned_width,
      .natural_width = natural_width,
      .minimum_width = minimum_width,
      .height = height,
      .line_count = pango_layout_get_line_count(layout.get()),
      .unknown_glyphs = unknown_glyphs,
      .links = {},
      .images = {},
  };

  LayoutIterPtr iterator{pango_layout_get_iter(layout.get())};
  do {
    PangoLayoutLine *line = pango_layout_iter_get_line_readonly(iterator.get());
    int y_start = 0;
    int y_end = 0;
    pango_layout_iter_get_line_yrange(iterator.get(), &y_start, &y_end);
    const int line_start = pango_layout_line_get_start_index(line);
    const int line_end = line_start + pango_layout_line_get_length(line);
    for (const auto &link : prepared.links) {
      const int range_start = std::max(line_start, static_cast<int>(link.bytes.start));
      const int range_end = std::min(line_end, static_cast<int>(link.bytes.end));
      if (range_start >= range_end) {
        continue;
      }
      int *ranges = nullptr;
      int range_count = 0;
      pango_layout_line_get_x_ranges(line, range_start, range_end, &ranges, &range_count);
      for (int range = 0; range < range_count; ++range) {
        const int left = pixel_floor(std::min(ranges[range * 2], ranges[range * 2 + 1]));
        const int right = pixel_ceil(std::max(ranges[range * 2], ranges[range * 2 + 1]));
        composition.links.push_back(RichLinkRectangle{
            .bounds = {left, pixel_floor(y_start), right - left,
                       pixel_ceil(y_end) - pixel_floor(y_start)},
            .action_id = link.action_id,
            .accessible_label = link.accessible_label,
        });
      }
      g_free(ranges);
    }
  } while (pango_layout_iter_next_line(iterator.get()));

  for (const auto &image : prepared.images) {
    PangoRectangle position{};
    pango_layout_index_to_pos(layout.get(), static_cast<int>(image.index), &position);
    const int line_height = pixel_ceil(position.height);
    composition.images.push_back(RichInlineImageRectangle{
        .bounds = {pixel_floor(position.x),
                   pixel_floor(position.y) + std::max(0, (line_height - image.height) / 2),
                   image.width, image.height},
        .resource_id = image.resource_id,
        .role = image.role,
        .accessible_label = image.accessible_label,
    });
  }

  return composition;
}

std::expected<RichTextSurface, RichTextError>
PangoTextBook::rasterize(const RichText &rich_text, int assigned_width,
                         const std::vector<ImageResource> &resources) const {
  auto measured = compose(rich_text, assigned_width);
  if (!measured) {
    return std::unexpected(measured.error());
  }
  cairo_surface_t *surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, assigned_width, measured->height);
  if (surface == nullptr || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    if (surface != nullptr) {
      cairo_surface_destroy(surface);
    }
    return std::unexpected(error(RichTextErrorCode::invalid_surface, "/surface",
                                 "Cairo image surface creation failed"));
  }
  cairo_t *context = cairo_create(surface);
  if (context == nullptr || cairo_status(context) != CAIRO_STATUS_SUCCESS) {
    if (context != nullptr) {
      cairo_destroy(context);
    }
    cairo_surface_destroy(surface);
    return std::unexpected(error(RichTextErrorCode::invalid_surface, "/surface",
                                 "Cairo drawing context creation failed"));
  }
  cairo_set_operator(context, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(context, 0.0, 0.0, 0.0, 0.0);
  cairo_paint(context);
  cairo_set_operator(context, CAIRO_OPERATOR_OVER);
  auto drawn = compose_with_cairo(rich_text, assigned_width, context);
  cairo_destroy(context);
  if (!drawn) {
    cairo_surface_destroy(surface);
    return std::unexpected(drawn.error());
  }
  cairo_surface_flush(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  const auto data_size =
      static_cast<std::size_t>(stride) * static_cast<std::size_t>(measured->height);
  auto pixels = cairo_argb32_to_rgba(
      std::span<const std::uint8_t>{cairo_image_surface_get_data(surface), data_size},
      assigned_width, measured->height, stride);
  cairo_surface_destroy(surface);
  if (!pixels) {
    return std::unexpected(pixels.error());
  }

  for (const auto &image : drawn->images) {
    const auto resource = std::ranges::find_if(resources, [&image](const ImageResource &candidate) {
      return candidate.id == image.resource_id;
    });
    const auto role = impl_->image_roles.find(image.role);
    if (resource == resources.end() || resource->pixels == nullptr || resource->width == 0 ||
        resource->height == 0 || role == impl_->image_roles.end()) {
      return std::unexpected(error(RichTextErrorCode::missing_resource, "/content",
                                   "inline image resource or role is unavailable"));
    }
    const auto expected_bytes = static_cast<std::size_t>(resource->width) * resource->height * 4U;
    if (resource->pixels->size() != expected_bytes) {
      return std::unexpected(error(RichTextErrorCode::missing_resource, "/content",
                                   "inline image resource has invalid pixel data"));
    }
    const auto prepared =
        prepare_image_pixels(*resource, role->second, image.bounds.width, image.bounds.height);
    for (int y = 0; y < image.bounds.height; ++y) {
      const int destination_y = image.bounds.y + y;
      if (destination_y < 0 || destination_y >= measured->height) {
        continue;
      }
      for (int x = 0; x < image.bounds.width; ++x) {
        const int destination_x = image.bounds.x + x;
        if (destination_x < 0 || destination_x >= assigned_width) {
          continue;
        }
        const std::size_t source_index =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.bounds.width) +
             static_cast<std::size_t>(x)) *
            4U;
        const std::size_t destination_index =
            (static_cast<std::size_t>(destination_y) * static_cast<std::size_t>(assigned_width) +
             static_cast<std::size_t>(destination_x)) *
            4U;
        source_over(std::span<std::uint8_t, 4>{pixels->data() + destination_index, 4},
                    std::span<const std::uint8_t, 4>{prepared.data() + source_index, 4});
      }
    }
  }

  return RichTextSurface{assigned_width, measured->height, std::move(*pixels), std::move(*drawn)};
}

} // namespace gisland
