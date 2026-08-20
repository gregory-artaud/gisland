#include "gisland/raylib_renderer.hpp"

#include <GL/gl.h>
#include <raylib.h>
#include <rlgl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace gisland {
namespace {

struct FontKey {
  std::filesystem::path path;
  int base_size;

  auto operator<=>(const FontKey &) const = default;
};

struct PreparedImageKey {
  std::string resource_id;
  int width;
  int height;
  ImageFit fit;
  ImageShape shape;
  int radius;

  auto operator<=>(const PreparedImageKey &) const = default;
};

[[nodiscard]] int base_size(const TypographyRole &role) {
  return std::max(1, static_cast<int>(std::ceil(role.size)));
}

[[nodiscard]] RendererError renderer_error(RendererErrorCode code, std::filesystem::path resource,
                                           std::string message) {
  return RendererError{code, std::move(resource), std::move(message)};
}

[[nodiscard]] Color color(Rgba value) {
  return Color{value.red, value.green, value.blue, value.alpha};
}

void ring_vertex(Vector2 center, float angle, float radius, Color tint, unsigned char alpha) {
  const auto blended_alpha = static_cast<unsigned char>(
      (static_cast<unsigned int>(tint.a) * static_cast<unsigned int>(alpha)) / 255U);
  rlColor4ub(tint.r, tint.g, tint.b, blended_alpha);
  rlVertex2f(center.x + (std::cos(angle * DEG2RAD) * radius),
             center.y + (std::sin(angle * DEG2RAD) * radius));
}

void draw_antialiased_ring(Vector2 center, float inner_radius, float outer_radius,
                           float start_angle, float end_angle, Color tint) {
  constexpr float feather = 1.0F;
  constexpr float degrees_per_segment = 4.0F;
  const float sweep = end_angle - start_angle;
  const int segments = std::max(1, static_cast<int>(std::ceil(sweep / degrees_per_segment)));
  const float step = sweep / static_cast<float>(segments);
  const std::array radii{inner_radius - feather, inner_radius, outer_radius,
                         outer_radius + feather};
  constexpr std::array<unsigned char, 4> alphas{0, 255, 255, 0};

  rlSetTexture(0);
  rlBegin(RL_QUADS);
  for (int segment = 0; segment < segments; ++segment) {
    const float angle = start_angle + (static_cast<float>(segment) * step);
    const float next_angle = angle + step;
    for (std::size_t band = 0; band + 1 < radii.size(); ++band) {
      ring_vertex(center, angle, radii[band + 1], tint, alphas[band + 1]);
      ring_vertex(center, angle, radii[band], tint, alphas[band]);
      ring_vertex(center, next_angle, radii[band], tint, alphas[band]);
      ring_vertex(center, next_angle, radii[band + 1], tint, alphas[band + 1]);
    }
  }
  rlEnd();
}

void draw_antialiased_cap(Vector2 center, float radius, Color tint) {
  constexpr float feather = 1.0F;
  constexpr int segments = 24;
  const float core_radius = radius;
  const float outer_radius = radius + feather;
  const float step = 360.0F / static_cast<float>(segments);

  DrawCircleV(center, core_radius, tint);
  rlSetTexture(0);
  rlBegin(RL_QUADS);
  for (int segment = 0; segment < segments; ++segment) {
    const float angle = static_cast<float>(segment) * step;
    const float next_angle = angle + step;
    ring_vertex(center, angle, outer_radius, tint, 0);
    ring_vertex(center, angle, core_radius, tint, 255);
    ring_vertex(center, next_angle, core_radius, tint, 255);
    ring_vertex(center, next_angle, outer_radius, tint, 0);
  }
  rlEnd();
}

void draw_antialiased_disc(Vector2 center, float radius, Color tint) {
  constexpr float feather = 1.0F;
  constexpr int segments = 24;
  const float core_radius = std::max(0.0F, radius - feather);
  const float step = 360.0F / static_cast<float>(segments);

  DrawCircleV(center, core_radius, tint);
  rlSetTexture(0);
  rlBegin(RL_QUADS);
  for (int segment = 0; segment < segments; ++segment) {
    const float angle = static_cast<float>(segment) * step;
    const float next_angle = angle + step;
    ring_vertex(center, angle, radius, tint, 0);
    ring_vertex(center, angle, core_radius, tint, 255);
    ring_vertex(center, next_angle, core_radius, tint, 255);
    ring_vertex(center, next_angle, radius, tint, 0);
  }
  rlEnd();
}

[[nodiscard]] float indicator_easing(float progress, Easing easing) {
  const float value = std::clamp(progress, 0.0F, 1.0F);
  switch (easing) {
  case Easing::linear:
    return value;
  case Easing::ease_in:
    return value * value;
  case Easing::ease_out:
    return 1.0F - ((1.0F - value) * (1.0F - value));
  case Easing::ease_in_out:
    return value < 0.5F ? 2.0F * value * value
                        : 1.0F - (std::pow(-2.0F * value + 2.0F, 2.0F) / 2.0F);
  }
  return value;
}

void draw_indicator_aura(Vector2 center, float core_radius, double radius, double intensity,
                         double opacity, Rgba tint) {
  constexpr int layers = 10;
  const float extent = static_cast<float>(radius * std::clamp(intensity, 0.0, 1.0));
  if (extent <= 0.0F || opacity <= 0.0) {
    return;
  }
  for (int layer = layers; layer >= 1; --layer) {
    const float fraction = static_cast<float>(layer) / static_cast<float>(layers);
    const double layer_opacity =
        opacity * (1.0 - static_cast<double>(fraction)) / static_cast<double>(layers / 2);
    Rgba layer_color = tint;
    layer_color.alpha = static_cast<std::uint8_t>(
        std::clamp(std::lround(static_cast<double>(tint.alpha) * layer_opacity), 0L, 255L));
    draw_antialiased_disc(center, core_radius + (extent * fraction), color(layer_color));
  }
}

[[nodiscard]] std::expected<Rect, RendererError>
checked_rect(std::int64_t x, std::int64_t y, std::int64_t width, std::int64_t height) {
  constexpr auto minimum = static_cast<std::int64_t>(std::numeric_limits<int>::min());
  constexpr auto maximum = static_cast<std::int64_t>(std::numeric_limits<int>::max());
  if (x < minimum || x > maximum || y < minimum || y > maximum || width < 0 || width > maximum ||
      height < 0 || height > maximum || x + width > maximum || y + height > maximum) {
    return std::unexpected(renderer_error(RendererErrorCode::invalid_geometry, {},
                                          "render coordinates exceed integer bounds"));
  }
  return Rect{static_cast<int>(x), static_cast<int>(y), static_cast<int>(width),
              static_cast<int>(height)};
}

[[nodiscard]] std::expected<Rect, RendererError> translated(Rect value, RenderOrigin origin) {
  return checked_rect(static_cast<std::int64_t>(value.x) + origin.x,
                      static_cast<std::int64_t>(value.y) + origin.y, value.width, value.height);
}

[[nodiscard]] Rectangle rectangle(Rect value) {
  return Rectangle{static_cast<float>(value.x), static_cast<float>(value.y),
                   static_cast<float>(value.width), static_cast<float>(value.height)};
}

[[nodiscard]] float roundness(Rect bounds, int radius) {
  const int short_edge = std::min(bounds.width, bounds.height);
  if (short_edge <= 0) {
    return 0.0F;
  }
  const double diameter = std::max(0.0, static_cast<double>(radius) * 2.0);
  return static_cast<float>(std::clamp(diameter / static_cast<double>(short_edge), 0.0, 1.0));
}

[[nodiscard]] PreparedImageKey image_key(const ImageDrawCommand &command) {
  return PreparedImageKey{
      command.resource_id,   command.bounds.width,
      command.bounds.height, command.style.fit,
      command.style.shape,   static_cast<int>(std::lround(command.style.radius))};
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

[[nodiscard]] std::vector<std::uint8_t> prepare_image_pixels(const ImageResource &resource,
                                                             const ImageDrawCommand &command) {
  const int output_width = command.bounds.width;
  const int output_height = command.bounds.height;
  std::vector<std::uint8_t> output(static_cast<std::size_t>(output_width) *
                                   static_cast<std::size_t>(output_height) * 4U);
  const double scale_x = static_cast<double>(output_width) / resource.width;
  const double scale_y = static_cast<double>(output_height) / resource.height;
  const double scale = command.style.fit == ImageFit::cover ? std::max(scale_x, scale_y)
                                                            : std::min(scale_x, scale_y);
  const double rendered_width = static_cast<double>(resource.width) * scale;
  const double rendered_height = static_cast<double>(resource.height) * scale;
  const double offset_x = (static_cast<double>(output_width) - rendered_width) / 2.0;
  const double offset_y = (static_cast<double>(output_height) - rendered_height) / 2.0;

  const auto source_channel = [&](int x, int y, std::size_t channel) {
    const std::size_t index =
        (static_cast<std::size_t>(y) * resource.width + static_cast<std::size_t>(x)) * 4U + channel;
    return static_cast<double>(resource.pixels->at(index));
  };
  for (int y = 0; y < output_height; ++y) {
    for (int x = 0; x < output_width; ++x) {
      const std::size_t output_index =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(output_width) +
           static_cast<std::size_t>(x)) *
          4U;
      const double center_x = static_cast<double>(x) + 0.5;
      const double center_y = static_cast<double>(y) + 0.5;
      if (center_x < offset_x || center_x >= offset_x + rendered_width || center_y < offset_y ||
          center_y >= offset_y + rendered_height ||
          !inside_mask(x, y, output_width, output_height, command.style.shape,
                       command.style.radius)) {
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

[[nodiscard]] std::expected<void, RendererError> draw_shadow(const RoundedView &view,
                                                             RenderOrigin origin) {
  if (view.shadow.color.alpha == 0) {
    return {};
  }
  const int layers = std::max(1, view.shadow.blur + 1);
  Rgba layer_color = view.shadow.color;
  layer_color.alpha =
      static_cast<std::uint8_t>(std::max(1, static_cast<int>(view.shadow.color.alpha) / layers));
  std::vector<std::pair<Rect, int>> layer_geometry;
  layer_geometry.reserve(static_cast<std::size_t>(layers));
  for (int blur = view.shadow.blur; blur >= 0; --blur) {
    const auto expansion = static_cast<std::int64_t>(view.shadow.spread) + blur;
    auto bounds =
        checked_rect(static_cast<std::int64_t>(view.bounds.x) + view.shadow.offset_x - expansion,
                     static_cast<std::int64_t>(view.bounds.y) + view.shadow.offset_y - expansion,
                     static_cast<std::int64_t>(view.bounds.width) + (2 * expansion),
                     static_cast<std::int64_t>(view.bounds.height) + (2 * expansion));
    if (!bounds) {
      return std::unexpected(bounds.error());
    }
    auto rendered_bounds = translated(*bounds, origin);
    if (!rendered_bounds) {
      return std::unexpected(rendered_bounds.error());
    }
    const auto radius = static_cast<std::int64_t>(view.radius) + expansion;
    if (radius < std::numeric_limits<int>::min() || radius > std::numeric_limits<int>::max()) {
      return std::unexpected(renderer_error(RendererErrorCode::invalid_geometry, {},
                                            "shadow radius exceeds integer bounds"));
    }
    layer_geometry.emplace_back(*rendered_bounds, static_cast<int>(radius));
  }

  rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA, RL_ONE, RL_ONE_MINUS_SRC_ALPHA,
                            RL_FUNC_ADD, RL_FUNC_ADD);
  BeginBlendMode(BLEND_CUSTOM_SEPARATE);
  for (const auto &[bounds, radius] : layer_geometry) {
    DrawRectangleRounded(rectangle(bounds), roundness(bounds, radius), 16, color(layer_color));
  }
  EndBlendMode();
  return {};
}

[[nodiscard]] std::string encode_utf8(char32_t codepoint) {
  std::string result;
  if (codepoint <= 0x7FU) {
    result.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FFU) {
    result.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
    result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else if (codepoint <= 0xFFFFU) {
    result.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
    result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else {
    result.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
    result.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
    result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  }
  return result;
}

template <typename Visitor>
[[nodiscard]] bool visit_utf8(std::string_view text, Visitor &&visitor) {
  for (std::size_t index = 0; index < text.size();) {
    const auto first = static_cast<unsigned char>(text[index]);
    char32_t codepoint = 0;
    std::size_t length = 0;
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
        codepoint > 0x10FFFFU) {
      return false;
    }
    if (!visitor(codepoint)) {
      return false;
    }
    index += length;
  }
  return true;
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

class Scissor final {
public:
  explicit Scissor(Rect clip) { BeginScissorMode(clip.x, clip.y, clip.width, clip.height); }

  Scissor(const Scissor &) = delete;
  Scissor &operator=(const Scissor &) = delete;
  ~Scissor() { EndScissorMode(); }
};

} // namespace

struct RaylibFontBook::Impl {
  std::map<std::string, std::filesystem::path, std::less<>> resolved_resources;
  std::map<FontKey, Font> fonts;
  Texture2D context_marker{};
  Color marker_color{};

  [[nodiscard]] bool owns_current_context() const {
    if (!IsWindowReady() || context_marker.id == 0 || glIsTexture(context_marker.id) != GL_TRUE) {
      return false;
    }
    GLint previous_texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glBindTexture(GL_TEXTURE_2D, context_marker.id);
    GLint width = 0;
    GLint height = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    std::array<GLubyte, 4> pixel{};
    if (width == 1 && height == 1) {
      glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    }
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
    return width == 1 && height == 1 && pixel[0] == marker_color.r && pixel[1] == marker_color.g &&
           pixel[2] == marker_color.b && pixel[3] == marker_color.a;
  }

  ~Impl() {
    const bool release_gpu_resources = owns_current_context();
    for (auto &[key, font] : fonts) {
      static_cast<void>(key);
      if (release_gpu_resources) {
        UnloadFont(font);
      } else {
        UnloadFontData(font.glyphs, font.glyphCount);
        MemFree(font.recs);
        font.glyphs = nullptr;
        font.recs = nullptr;
      }
    }
    if (release_gpu_resources) {
      UnloadTexture(context_marker);
    }
  }

  [[nodiscard]] std::expected<const Font *, RendererError> find(std::string_view resource,
                                                                const TypographyRole &role) const {
    const auto resolved = resolved_resources.find(resource);
    if (resolved == resolved_resources.end()) {
      return std::unexpected(renderer_error(RendererErrorCode::font_not_loaded,
                                            std::filesystem::path{resource},
                                            "font resource is not loaded"));
    }
    const FontKey key{resolved->second, base_size(role)};
    const auto font = fonts.find(key);
    if (font == fonts.end()) {
      return std::unexpected(renderer_error(RendererErrorCode::font_not_loaded, resolved->second,
                                            "font size is not loaded"));
    }
    return &font->second;
  }
};

struct RaylibImageBook::Impl {
  struct PreparedSignature {
    std::uint32_t source_width;
    std::uint32_t source_height;
    std::shared_ptr<const std::vector<std::uint8_t>> pixels;
    int output_width;
    int output_height;
    ImageRole style;
    std::size_t texture_index;
  };

  std::map<std::string, ImageResource, std::less<>> resources;
  std::map<PreparedImageKey, std::size_t> bindings;
  std::vector<PreparedSignature> signatures;
  std::vector<Texture2D> textures;

  ~Impl() {
    if (!IsWindowReady()) {
      return;
    }
    for (const auto texture : textures) {
      UnloadTexture(texture);
    }
  }

  [[nodiscard]] std::expected<const Texture2D *, RendererError>
  find(const ImageDrawCommand &command) const {
    const auto binding = bindings.find(image_key(command));
    if (binding == bindings.end() || binding->second >= textures.size()) {
      return std::unexpected(renderer_error(RendererErrorCode::missing_resource,
                                            std::filesystem::path{command.resource_id},
                                            "prepared image resource is not loaded"));
    }
    return &textures[binding->second];
  }
};

struct RaylibRichTextBook::Impl {
  struct PreparedSignature {
    RichText rich_text;
    int width;
    int height;
    std::size_t texture_index;
  };

  const PangoTextBook *text{};
  std::vector<ImageResource> resources;
  std::vector<PreparedSignature> signatures;
  std::vector<Texture2D> textures;

  ~Impl() {
    if (!IsWindowReady()) {
      return;
    }
    for (const auto texture : textures) {
      UnloadTexture(texture);
    }
  }

  [[nodiscard]] std::expected<const Texture2D *, RendererError>
  find(const RichTextDrawCommand &command) const {
    const auto signature = std::ranges::find_if(signatures, [&command](const auto &candidate) {
      return candidate.width == command.bounds.width && candidate.height == command.bounds.height &&
             candidate.rich_text == command.rich_text;
    });
    if (signature == signatures.end() || signature->texture_index >= textures.size()) {
      return std::unexpected(renderer_error(RendererErrorCode::missing_resource, {},
                                            "prepared rich text texture is not loaded"));
    }
    return &textures[signature->texture_index];
  }
};

RaylibFontBook::RaylibFontBook(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

RaylibFontBook::RaylibFontBook(RaylibFontBook &&) noexcept = default;

RaylibFontBook &RaylibFontBook::operator=(RaylibFontBook &&) noexcept = default;

RaylibFontBook::~RaylibFontBook() = default;

std::expected<RaylibFontBook, RendererError>
RaylibFontBook::load(const Theme &theme, const std::filesystem::path &asset_root) {
  if (!IsWindowReady()) {
    return std::unexpected(renderer_error(RendererErrorCode::window_not_ready, {},
                                          "raylib window must exist before loading fonts"));
  }

  auto impl = std::make_unique<Impl>();
  std::uint64_t marker_seed = reinterpret_cast<std::uintptr_t>(impl.get());
  marker_seed ^= marker_seed >> 30U;
  marker_seed *= 0xBF58476D1CE4E5B9ULL;
  marker_seed ^= marker_seed >> 27U;
  impl->marker_color = Color{static_cast<unsigned char>((marker_seed >> 0U) | 1U),
                             static_cast<unsigned char>((marker_seed >> 8U) | 1U),
                             static_cast<unsigned char>((marker_seed >> 16U) | 1U), 255};
  ::Image marker_image = GenImageColor(1, 1, impl->marker_color);
  impl->context_marker = LoadTextureFromImage(marker_image);
  UnloadImage(marker_image);
  if (!IsTextureValid(impl->context_marker)) {
    return std::unexpected(renderer_error(RendererErrorCode::font_load_failed, {},
                                          "raylib failed to create a context marker texture"));
  }
  std::error_code filesystem_error;
  for (const auto &[name, resource] : theme.fonts()) {
    static_cast<void>(name);
    const std::filesystem::path declared{resource};
    const std::filesystem::path resolved =
        (declared.is_absolute() ? declared : asset_root / declared).lexically_normal();
    const bool exists = std::filesystem::exists(resolved, filesystem_error);
    if (filesystem_error) {
      return std::unexpected(renderer_error(RendererErrorCode::invalid_resource, resolved,
                                            "font resource could not be inspected"));
    }
    if (!exists) {
      return std::unexpected(renderer_error(RendererErrorCode::missing_resource, resolved,
                                            "font resource does not exist"));
    }
    const bool regular_file = std::filesystem::is_regular_file(resolved, filesystem_error);
    if (filesystem_error || !regular_file) {
      return std::unexpected(renderer_error(RendererErrorCode::invalid_resource, resolved,
                                            "font resource is not a regular readable file"));
    }
    impl->resolved_resources.emplace(resource, resolved);
  }

  const TypographyRole &body = theme.typography().at("body");
  std::map<FontKey, std::set<int>> requests;
  const auto request_font = [&](std::string_view resource, const TypographyRole &role) {
    const auto resolved = impl->resolved_resources.find(resource);
    requests[FontKey{resolved->second, base_size(role)}];
  };
  for (const auto &[name, resource] : theme.fonts()) {
    static_cast<void>(name);
    request_font(resource, body);
  }
  for (const auto &[name, role] : theme.typography()) {
    static_cast<void>(name);
    request_font(theme.fonts().at(role.font), role);
  }

  std::map<std::filesystem::path, std::set<int>> icon_codepoints;
  for (const auto &[name, icon] : theme.icons()) {
    static_cast<void>(name);
    const auto &resource = theme.fonts().at(icon.font);
    const auto resolved = impl->resolved_resources.find(resource);
    icon_codepoints[resolved->second].insert(static_cast<int>(icon.codepoint));
    request_font(resource, body);
  }

  for (auto &[key, codepoints] : requests) {
    add_text_codepoints(codepoints);
    if (const auto icons = icon_codepoints.find(key.path); icons != icon_codepoints.end()) {
      codepoints.insert(icons->second.begin(), icons->second.end());
    }
    const std::vector<int> pinned_codepoints{codepoints.begin(), codepoints.end()};
    Font font = LoadFontEx(key.path.c_str(), key.base_size, pinned_codepoints.data(),
                           static_cast<int>(pinned_codepoints.size()));
    const bool default_fallback = font.texture.id == GetFontDefault().texture.id;
    if (!IsFontValid(font) || !IsTextureValid(font.texture) || default_fallback) {
      if (!default_fallback && IsFontValid(font)) {
        UnloadFont(font);
      }
      return std::unexpected(renderer_error(RendererErrorCode::font_load_failed, key.path,
                                            "raylib failed to load the font resource"));
    }
    impl->fonts.emplace(key, font);
  }

  return RaylibFontBook{std::move(impl)};
}

bool RaylibFontBook::supports_text(std::string_view font_resource, const TypographyRole &role,
                                   std::string_view text) const {
  return visit_utf8(text, [this, font_resource, &role](char32_t codepoint) {
    return supports_codepoint(font_resource, role, codepoint);
  });
}

bool RaylibFontBook::supports_codepoint(std::string_view font_resource, const TypographyRole &role,
                                        char32_t codepoint) const {
  const auto font = impl_->find(font_resource, role);
  if (!font) {
    return false;
  }
  return std::any_of(
      (*font)->glyphs, (*font)->glyphs + (*font)->glyphCount,
      [codepoint](const GlyphInfo &glyph) { return glyph.value == static_cast<int>(codepoint); });
}

MeasuredGlyphs RaylibFontBook::measure_text(std::string_view font_resource,
                                            const TypographyRole &role,
                                            std::string_view text) const {
  const auto font = impl_->find(font_resource, role);
  if (!font) {
    return {};
  }
  const std::string terminated{text};
  const Vector2 measured =
      MeasureTextEx(**font, terminated.c_str(), static_cast<float>(role.size), 0.0F);
  return MeasuredGlyphs{static_cast<double>(measured.x),
                        static_cast<double>(measured.y) * role.line_height};
}

MeasuredGlyphs RaylibFontBook::measure_codepoint(std::string_view font_resource,
                                                 const TypographyRole &role,
                                                 char32_t codepoint) const {
  return measure_text(font_resource, role, encode_utf8(codepoint));
}

std::size_t RaylibFontBook::loaded_font_count() const noexcept { return impl_->fonts.size(); }

RaylibImageBook::RaylibImageBook(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

RaylibImageBook::RaylibImageBook(RaylibImageBook &&) noexcept = default;

RaylibImageBook &RaylibImageBook::operator=(RaylibImageBook &&) noexcept = default;

RaylibImageBook::~RaylibImageBook() = default;

std::expected<RaylibImageBook, RendererError>
RaylibImageBook::load(const std::vector<ImageResource> &resources) {
  if (!IsWindowReady()) {
    return std::unexpected(renderer_error(RendererErrorCode::window_not_ready, {},
                                          "raylib window must exist before loading images"));
  }
  auto impl = std::make_unique<Impl>();
  for (const auto &resource : resources) {
    const std::size_t expected = static_cast<std::size_t>(resource.width) * resource.height * 4U;
    if (resource.width == 0 || resource.height == 0 || resource.pixels == nullptr ||
        resource.pixels->size() != expected || resource.format != ImageFormat::rgba8 ||
        !impl->resources.emplace(resource.id, resource).second) {
      return std::unexpected(renderer_error(RendererErrorCode::invalid_resource,
                                            std::filesystem::path{resource.id},
                                            "invalid typed image resource"));
    }
  }
  return RaylibImageBook{std::move(impl)};
}

std::expected<void, RendererError> RaylibImageBook::prepare(const LayoutPlan &plan) {
  for (const auto &content : plan.content) {
    const auto *command = std::get_if<ImageDrawCommand>(&content);
    if (command == nullptr) {
      continue;
    }
    const auto key = image_key(*command);
    if (impl_->bindings.contains(key)) {
      continue;
    }
    const auto resource = impl_->resources.find(command->resource_id);
    if (resource == impl_->resources.end()) {
      return std::unexpected(renderer_error(RendererErrorCode::missing_resource,
                                            std::filesystem::path{command->resource_id},
                                            "image resource is not part of the context"));
    }
    if (command->bounds.width <= 0 || command->bounds.height <= 0) {
      return std::unexpected(renderer_error(RendererErrorCode::invalid_geometry,
                                            std::filesystem::path{command->resource_id},
                                            "prepared image dimensions must be positive"));
    }
    const auto shared = std::ranges::find_if(impl_->signatures, [&](const auto &signature) {
      return signature.source_width == resource->second.width &&
             signature.source_height == resource->second.height &&
             signature.output_width == command->bounds.width &&
             signature.output_height == command->bounds.height &&
             signature.style == command->style && *signature.pixels == *resource->second.pixels;
    });
    if (shared != impl_->signatures.end()) {
      impl_->bindings.emplace(key, shared->texture_index);
      continue;
    }
    auto pixels = prepare_image_pixels(resource->second, *command);
    ::Image image{pixels.data(), command->bounds.width, command->bounds.height, 1,
                  PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Texture2D texture = LoadTextureFromImage(image);
    if (!IsTextureValid(texture)) {
      return std::unexpected(renderer_error(RendererErrorCode::image_load_failed,
                                            std::filesystem::path{command->resource_id},
                                            "raylib failed to load the prepared image texture"));
    }
    SetTextureFilter(texture, TEXTURE_FILTER_BILINEAR);
    const std::size_t index = impl_->textures.size();
    impl_->textures.push_back(texture);
    impl_->bindings.emplace(key, index);
    impl_->signatures.push_back(Impl::PreparedSignature{
        resource->second.width, resource->second.height, resource->second.pixels,
        command->bounds.width, command->bounds.height, command->style, index});
  }
  return {};
}

std::size_t RaylibImageBook::loaded_texture_count() const noexcept {
  return impl_->textures.size();
}

RaylibRichTextBook::RaylibRichTextBook(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

RaylibRichTextBook::RaylibRichTextBook(RaylibRichTextBook &&) noexcept = default;

RaylibRichTextBook &RaylibRichTextBook::operator=(RaylibRichTextBook &&) noexcept = default;

RaylibRichTextBook::~RaylibRichTextBook() = default;

std::expected<RaylibRichTextBook, RendererError>
RaylibRichTextBook::load(const PangoTextBook &text, const std::vector<ImageResource> &resources) {
  if (!IsWindowReady()) {
    return std::unexpected(renderer_error(RendererErrorCode::window_not_ready, {},
                                          "raylib window must exist before loading rich text"));
  }
  auto impl = std::make_unique<Impl>();
  impl->text = &text;
  impl->resources = resources;
  for (const auto &resource : impl->resources) {
    const std::size_t expected = static_cast<std::size_t>(resource.width) * resource.height * 4U;
    if (resource.width == 0 || resource.height == 0 || resource.pixels == nullptr ||
        resource.pixels->size() != expected || resource.format != ImageFormat::rgba8) {
      return std::unexpected(renderer_error(RendererErrorCode::invalid_resource,
                                            std::filesystem::path{resource.id},
                                            "invalid typed inline image resource"));
    }
  }
  return RaylibRichTextBook{std::move(impl)};
}

std::expected<void, RendererError> RaylibRichTextBook::prepare(const LayoutPlan &plan) {
  for (const auto &content : plan.content) {
    const auto *command = std::get_if<RichTextDrawCommand>(&content);
    if (command == nullptr) {
      continue;
    }
    if (command->bounds.width <= 0 || command->bounds.height <= 0) {
      return std::unexpected(renderer_error(RendererErrorCode::invalid_geometry, {},
                                            "rich text texture dimensions must be positive"));
    }
    const auto existing =
        std::ranges::find_if(impl_->signatures, [&command](const auto &candidate) {
          return candidate.width == command->bounds.width &&
                 candidate.height == command->bounds.height &&
                 candidate.rich_text == command->rich_text;
        });
    if (existing != impl_->signatures.end()) {
      continue;
    }
    auto surface =
        impl_->text->rasterize(command->rich_text, command->bounds.width, impl_->resources);
    if (!surface) {
      return std::unexpected(
          renderer_error(RendererErrorCode::image_load_failed, {}, surface.error().message));
    }
    if (surface->height != command->bounds.height) {
      return std::unexpected(renderer_error(RendererErrorCode::invalid_geometry, {},
                                            "rich text raster height differs from layout"));
    }
    ::Image image{surface->pixels.data(), surface->width, surface->height, 1,
                  PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Texture2D texture = LoadTextureFromImage(image);
    if (!IsTextureValid(texture)) {
      return std::unexpected(renderer_error(RendererErrorCode::image_load_failed, {},
                                            "raylib failed to load the rich text texture"));
    }
    SetTextureFilter(texture, TEXTURE_FILTER_BILINEAR);
    const std::size_t index = impl_->textures.size();
    impl_->textures.push_back(texture);
    impl_->signatures.push_back(Impl::PreparedSignature{command->rich_text, command->bounds.width,
                                                        command->bounds.height, index});
  }
  return {};
}

std::size_t RaylibRichTextBook::loaded_texture_count() const noexcept {
  return impl_->textures.size();
}

std::expected<void, RendererError> RaylibPainter::draw_surface(const LayoutPlan &plan,
                                                               RenderOrigin origin) const {
  if (!IsWindowReady()) {
    return std::unexpected(renderer_error(RendererErrorCode::window_not_ready, {},
                                          "raylib window is not ready for drawing"));
  }
  const auto &view = plan.view;
  auto rendered_bounds = translated(view.bounds, origin);
  if (!rendered_bounds) {
    return std::unexpected(rendered_bounds.error());
  }
  auto shadow = draw_shadow(view, origin);
  if (!shadow) {
    return shadow;
  }
  DrawRectangleRounded(rectangle(*rendered_bounds), roundness(*rendered_bounds, view.radius), 16,
                       color(view.surface));
  if (view.border > 0) {
    DrawRectangleRoundedLinesEx(rectangle(*rendered_bounds),
                                roundness(*rendered_bounds, view.radius), 16,
                                static_cast<float>(view.border), color(view.border_color));
  }
  return {};
}

std::expected<void, RendererError>
RaylibPainter::draw_content(const LayoutPlan &plan, RenderOrigin origin,
                            IndicatorAnimationState indicator_animation) const {
  if (!IsWindowReady()) {
    return std::unexpected(renderer_error(RendererErrorCode::window_not_ready, {},
                                          "raylib window is not ready for drawing"));
  }

  for (const auto &content : plan.content) {
    auto drawn = std::visit(
        [this, origin,
         indicator_animation](const auto &command) -> std::expected<void, RendererError> {
          if (command.clip.width <= 0 || command.clip.height <= 0) {
            return {};
          }
          auto rendered_clip = translated(command.clip, origin);
          if (!rendered_clip) {
            return std::unexpected(rendered_clip.error());
          }
          auto rendered_bounds = translated(command.bounds, origin);
          if (!rendered_bounds) {
            return std::unexpected(rendered_bounds.error());
          }
          if constexpr (std::is_same_v<std::decay_t<decltype(command)>, TextDrawCommand>) {
            const auto font = fonts_.impl_->find(command.font_resource, command.typography);
            if (!font) {
              return std::unexpected(font.error());
            }
            const Scissor scissor{*rendered_clip};
            DrawTextEx(**font, command.text.c_str(),
                       Vector2{static_cast<float>(rendered_bounds->x),
                               static_cast<float>(rendered_bounds->y)},
                       static_cast<float>(command.typography.size), 0.0F, color(command.color));
          } else if constexpr (std::is_same_v<std::decay_t<decltype(command)>,
                                              RichTextDrawCommand>) {
            if (rich_text_ == nullptr) {
              return std::unexpected(renderer_error(RendererErrorCode::missing_resource, {},
                                                    "rich text texture book is unavailable"));
            }
            const auto texture = rich_text_->impl_->find(command);
            if (!texture) {
              return std::unexpected(texture.error());
            }
            const Scissor scissor{*rendered_clip};
            DrawTexture(**texture, rendered_bounds->x, rendered_bounds->y, WHITE);
          } else if constexpr (std::is_same_v<std::decay_t<decltype(command)>, IconDrawCommand>) {
            const auto font = fonts_.impl_->find(command.font_resource, command.typography);
            if (!font) {
              return std::unexpected(font.error());
            }
            const std::string glyph = encode_utf8(command.codepoint);
            const Scissor scissor{*rendered_clip};
            DrawTextEx(**font, glyph.c_str(),
                       Vector2{static_cast<float>(rendered_bounds->x),
                               static_cast<float>(rendered_bounds->y)},
                       static_cast<float>(command.typography.size), 0.0F, color(command.color));
          } else if constexpr (std::is_same_v<std::decay_t<decltype(command)>, ImageDrawCommand>) {
            if (images_ == nullptr) {
              return std::unexpected(renderer_error(RendererErrorCode::missing_resource,
                                                    std::filesystem::path{command.resource_id},
                                                    "image resource is not loaded"));
            }
            const auto texture = images_->impl_->find(command);
            if (!texture) {
              return std::unexpected(texture.error());
            }
            const Scissor scissor{*rendered_clip};
            DrawTexture(**texture, rendered_bounds->x, rendered_bounds->y, WHITE);
          } else if constexpr (std::is_same_v<std::decay_t<decltype(command)>,
                                              ProgressDrawCommand>) {
            auto rendered_track = translated(command.track, origin);
            auto rendered_fill = translated(command.fill, origin);
            if (!rendered_track) {
              return std::unexpected(rendered_track.error());
            }
            if (!rendered_fill) {
              return std::unexpected(rendered_fill.error());
            }
            const Scissor scissor{*rendered_clip};
            DrawRectangleRounded(rectangle(*rendered_track), 1.0F, 16, color(command.track_color));
            if (command.fill.width > 0 && command.fill.height > 0) {
              DrawRectangleRounded(rectangle(*rendered_fill), 1.0F, 16, color(command.fill_color));
            }
          } else if constexpr (std::is_same_v<std::decay_t<decltype(command)>,
                                              RingProgressDrawCommand>) {
            const Scissor scissor{*rendered_clip};
            const Vector2 center{static_cast<float>(rendered_bounds->x) +
                                     (static_cast<float>(rendered_bounds->width) / 2.0F),
                                 static_cast<float>(rendered_bounds->y) +
                                     (static_cast<float>(rendered_bounds->height) / 2.0F)};
            const float outer_radius =
                static_cast<float>(std::min(rendered_bounds->width, rendered_bounds->height)) /
                2.0F;
            const float thickness = static_cast<float>(command.thickness);
            const float inner_radius = outer_radius - thickness;
            rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA, RL_ONE,
                                      RL_ONE_MINUS_SRC_ALPHA, RL_FUNC_ADD, RL_FUNC_ADD);
            BeginBlendMode(BLEND_CUSTOM_SEPARATE);
            draw_antialiased_ring(center, inner_radius, outer_radius, -90.0F, 270.0F,
                                  color(command.track_color));
            if (command.value > 0.0) {
              const float end_angle = -90.0F + static_cast<float>(command.value * 360.0);
              draw_antialiased_ring(center, inner_radius, outer_radius, -90.0F, end_angle,
                                    color(command.fill_color));
              const float middle_radius = inner_radius + (thickness / 2.0F);
              const auto cap_center = [&](float angle) {
                return Vector2{center.x + (std::cos(angle * DEG2RAD) * middle_radius),
                               center.y + (std::sin(angle * DEG2RAD) * middle_radius)};
              };
              draw_antialiased_cap(cap_center(-90.0F), thickness / 2.0F, color(command.fill_color));
              draw_antialiased_cap(cap_center(end_angle), thickness / 2.0F,
                                   color(command.fill_color));
            }
            EndBlendMode();
          } else if constexpr (std::is_same_v<std::decay_t<decltype(command)>,
                                              IndicatorDrawCommand>) {
            const Scissor scissor{*rendered_clip};
            const Vector2 center{static_cast<float>(rendered_bounds->x) +
                                     (static_cast<float>(rendered_bounds->width) / 2.0F),
                                 static_cast<float>(rendered_bounds->y) +
                                     (static_cast<float>(rendered_bounds->height) / 2.0F)};
            const float radius =
                static_cast<float>(std::min(rendered_bounds->width, rendered_bounds->height)) /
                2.0F;
            rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA, RL_ONE,
                                      RL_ONE_MINUS_SRC_ALPHA, RL_FUNC_ADD, RL_FUNC_ADD);
            BeginBlendMode(BLEND_CUSTOM_SEPARATE);
            const auto has_effect = [&command](IndicatorEffect effect) {
              return std::ranges::find(command.effects, effect) != command.effects.end();
            };
            if (command.style) {
              const auto &style = *command.style;
              if (has_effect(IndicatorEffect::shadow)) {
                const Vector2 shadow_center{center.x + static_cast<float>(style.shadow.offset_x),
                                            center.y + static_cast<float>(style.shadow.offset_y)};
                draw_indicator_aura(shadow_center, radius, style.shadow.radius, 1.0,
                                    style.shadow.opacity, Rgba{0, 0, 0, 255});
              }
              if (has_effect(IndicatorEffect::glow)) {
                draw_indicator_aura(center, radius, style.glow.radius, style.glow.intensity,
                                    style.glow.opacity, command.color);
              }
              if (has_effect(IndicatorEffect::breathe)) {
                double intensity = style.reduced_motion.breathe_intensity;
                double opacity = style.reduced_motion.breathe_opacity;
                if (!indicator_animation.reduced_motion) {
                  const double duration =
                      static_cast<double>(style.breathe.duration.count()) / 1000.0;
                  const double cycle =
                      duration > 0.0 ? std::fmod(std::max(0.0, indicator_animation.elapsed_seconds),
                                                 duration) /
                                           duration
                                     : 0.0;
                  const float wave = static_cast<float>((1.0 - std::cos(cycle * 2.0 * PI)) / 2.0);
                  const double eased = indicator_easing(wave, style.breathe.easing);
                  intensity = std::lerp(style.breathe.minimum_intensity,
                                        style.breathe.maximum_intensity, eased);
                  opacity = std::lerp(style.breathe.minimum_opacity, style.breathe.maximum_opacity,
                                      eased);
                }
                draw_indicator_aura(center, radius, style.breathe.radius, intensity, opacity,
                                    command.color);
              }
            }
            draw_antialiased_disc(center, radius, color(command.color));
            EndBlendMode();
          } else {
            const Scissor scissor{*rendered_clip};
            DrawRectangleRounded(rectangle(*rendered_bounds), 1.0F, 16, color(command.color));
          }
          return {};
        },
        content);
    if (!drawn) {
      return drawn;
    }
  }
  return {};
}

} // namespace gisland
