#include "gisland/rlgl_painter.hpp"

#include "gisland/rlgl_renderer_model.hpp"
#include "gisland/rlgl_texture_books.hpp"

#include <rlgl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace gisland {
namespace {

constexpr float pi = 3.14159265358979323846F;

[[nodiscard]] std::expected<Rect, RlglPaintError>
checked_rect(std::int64_t x, std::int64_t y, std::int64_t width, std::int64_t height) {
  constexpr auto minimum = static_cast<std::int64_t>(std::numeric_limits<int>::min());
  constexpr auto maximum = static_cast<std::int64_t>(std::numeric_limits<int>::max());
  if (x < minimum || x > maximum || y < minimum || y > maximum || width < 0 || width > maximum ||
      height < 0 || height > maximum || x + width > maximum || y + height > maximum) {
    return std::unexpected(RlglPaintError::invalid_geometry);
  }
  return Rect{static_cast<int>(x), static_cast<int>(y), static_cast<int>(width),
              static_cast<int>(height)};
}

[[nodiscard]] std::expected<Rect, RlglPaintError> translated(Rect value, RenderOrigin origin) {
  return checked_rect(static_cast<std::int64_t>(value.x) + origin.x,
                      static_cast<std::int64_t>(value.y) + origin.y, value.width, value.height);
}

void vertex(Point point, Rgba color) {
  rlColor4ub(color.red, color.green, color.blue, color.alpha);
  rlVertex2f(point.x, point.y);
}

void draw_quads(const std::vector<Quad> &quads, Rgba color) {
  rlSetTexture(rlGetTextureIdDefault());
  rlBegin(RL_QUADS);
  for (const auto &quad : quads) {
    for (const auto point : quad.vertices) {
      vertex(point, color);
    }
  }
  rlEnd();
  rlSetTexture(0);
}

void draw_texture_quad(const RlglTexture &texture, Rect source, float x, float y, float width,
                       float height, Rgba color) {
  const float texture_width = static_cast<float>(texture.width());
  const float texture_height = static_cast<float>(texture.height());
  const float left = static_cast<float>(source.x) / texture_width;
  const float top = static_cast<float>(source.y) / texture_height;
  const float right = static_cast<float>(source.x + source.width) / texture_width;
  const float bottom = static_cast<float>(source.y + source.height) / texture_height;
  rlSetTexture(texture.id());
  rlBegin(RL_QUADS);
  rlColor4ub(color.red, color.green, color.blue, color.alpha);
  rlTexCoord2f(left, top);
  rlVertex2f(x, y);
  rlTexCoord2f(left, bottom);
  rlVertex2f(x, y + height);
  rlTexCoord2f(right, bottom);
  rlVertex2f(x + width, y + height);
  rlTexCoord2f(right, top);
  rlVertex2f(x + width, y);
  rlEnd();
  rlSetTexture(0);
}

void draw_texture(const RlglTexture &texture, Rect bounds) {
  draw_texture_quad(texture, {0, 0, texture.width(), texture.height()},
                    static_cast<float>(bounds.x), static_cast<float>(bounds.y),
                    static_cast<float>(bounds.width), static_cast<float>(bounds.height),
                    {255, 255, 255, 255});
}

[[nodiscard]] const FontAtlasGlyph *find_glyph(const FontAtlasData &atlas, char32_t codepoint) {
  const auto glyph = std::ranges::find(atlas.glyphs, codepoint, &FontAtlasGlyph::codepoint);
  return glyph == atlas.glyphs.end() ? nullptr : &*glyph;
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

[[nodiscard]] std::expected<void, RlglPaintError> draw_codepoint(const RlglFontBinding &font,
                                                                 char32_t codepoint, float x,
                                                                 float y, float size, Rgba color) {
  const auto *glyph = find_glyph(*font.atlas, codepoint);
  if (glyph == nullptr) {
    return std::unexpected(RlglPaintError::unsupported_command);
  }
  if (codepoint == U' ' || codepoint == U'\t') {
    return {};
  }
  const float scale = size / static_cast<float>(font.atlas->base_size);
  const float padding = static_cast<float>(font.atlas->padding);
  const Rect source{glyph->rectangle.x - font.atlas->padding,
                    glyph->rectangle.y - font.atlas->padding,
                    glyph->rectangle.width + 2 * font.atlas->padding,
                    glyph->rectangle.height + 2 * font.atlas->padding};
  draw_texture_quad(
      *font.texture, source, x + static_cast<float>(glyph->offset_x) * scale - padding * scale,
      y + static_cast<float>(glyph->offset_y) * scale - padding * scale,
      static_cast<float>(source.width) * scale, static_cast<float>(source.height) * scale, color);
  return {};
}

[[nodiscard]] std::expected<void, RlglPaintError> draw_text(const RlglFontBinding &font,
                                                            std::string_view text, float x, float y,
                                                            float size, Rgba color) {
  float offset_x{};
  float offset_y{};
  const float scale = size / static_cast<float>(font.atlas->base_size);
  const bool valid = visit_utf8(text, [&](char32_t codepoint) {
    if (codepoint == U'\n') {
      offset_x = 0.0F;
      offset_y += size + 2.0F;
      return true;
    }
    const auto *glyph = find_glyph(*font.atlas, codepoint);
    if (glyph == nullptr) {
      return false;
    }
    if (!draw_codepoint(font, codepoint, x + offset_x, y + offset_y, size, color)) {
      return false;
    }
    offset_x +=
        static_cast<float>(glyph->advance_x != 0 ? glyph->advance_x : glyph->rectangle.width) *
        scale;
    return true;
  });
  return valid ? std::expected<void, RlglPaintError>{}
               : std::unexpected(RlglPaintError::unsupported_command);
}

[[nodiscard]] std::expected<void, RlglPaintError> draw_rounded(Rect bounds, float radius,
                                                               Rgba color) {
  auto mesh = tessellate_rounded_rectangle(bounds, radius);
  if (!mesh) {
    return std::unexpected(RlglPaintError::invalid_geometry);
  }
  draw_quads(mesh->quads, color);
  return {};
}

void rectangle_quad(float left, float top, float right, float bottom, Rgba color) {
  draw_quads(
      {Quad{{Point{left, top}, Point{left, bottom}, Point{right, bottom}, Point{right, top}}}},
      color);
}

void draw_rounded_border(Rect bounds, float radius, float thickness, Rgba color) {
  const float left = static_cast<float>(bounds.x);
  const float top = static_cast<float>(bounds.y);
  const float right = left + static_cast<float>(bounds.width);
  const float bottom = top + static_cast<float>(bounds.height);
  if (radius <= 0.0F) {
    rectangle_quad(left - thickness, top - thickness, right + thickness, top, color);
    rectangle_quad(left - thickness, bottom, right + thickness, bottom + thickness, color);
    rectangle_quad(left - thickness, top, left, bottom, color);
    rectangle_quad(right, top, right + thickness, bottom, color);
    return;
  }

  const float inner_radius =
      std::min(radius, static_cast<float>(std::min(bounds.width, bounds.height)) / 2.0F);
  const float outer_radius = inner_radius + thickness;
  const std::array<Point, 4> centers{
      Point{left + inner_radius + 0.5F, top + inner_radius + 0.5F},
      Point{right - inner_radius - 0.5F, top + inner_radius + 0.5F},
      Point{right - inner_radius - 0.5F, bottom - inner_radius - 0.5F},
      Point{left + inner_radius + 0.5F, bottom - inner_radius - 0.5F}};
  constexpr std::array<float, 4> starts{180.0F, 270.0F, 0.0F, 90.0F};
  constexpr int segments = 16;
  constexpr float step = 90.0F / static_cast<float>(segments);
  std::vector<Quad> quads;
  quads.reserve(4U * static_cast<std::size_t>(segments) + 4U);
  for (std::size_t corner = 0; corner < centers.size(); ++corner) {
    for (int segment = 0; segment < segments; ++segment) {
      const float angle = (starts[corner] + static_cast<float>(segment) * step) * pi / 180.0F;
      const float next = angle + step * pi / 180.0F;
      quads.push_back({{Point{centers[corner].x + std::cos(angle) * inner_radius,
                              centers[corner].y + std::sin(angle) * inner_radius},
                        Point{centers[corner].x + std::cos(next) * inner_radius,
                              centers[corner].y + std::sin(next) * inner_radius},
                        Point{centers[corner].x + std::cos(next) * outer_radius,
                              centers[corner].y + std::sin(next) * outer_radius},
                        Point{centers[corner].x + std::cos(angle) * outer_radius,
                              centers[corner].y + std::sin(angle) * outer_radius}}});
    }
  }
  quads.push_back({{Point{left + inner_radius + 0.5F, top - thickness + 0.5F},
                    Point{left + inner_radius + 0.5F, top + 0.5F},
                    Point{right - inner_radius - 0.5F, top + 0.5F},
                    Point{right - inner_radius - 0.5F, top - thickness + 0.5F}}});
  quads.push_back({{Point{right - 0.5F, top + inner_radius + 0.5F},
                    Point{right - 0.5F, bottom - inner_radius - 0.5F},
                    Point{right + thickness - 0.5F, bottom - inner_radius - 0.5F},
                    Point{right + thickness - 0.5F, top + inner_radius + 0.5F}}});
  quads.push_back({{Point{left + inner_radius + 0.5F, bottom - 0.5F},
                    Point{left + inner_radius + 0.5F, bottom + thickness - 0.5F},
                    Point{right - inner_radius - 0.5F, bottom + thickness - 0.5F},
                    Point{right - inner_radius - 0.5F, bottom - 0.5F}}});
  quads.push_back({{Point{left - thickness + 0.5F, top + inner_radius + 0.5F},
                    Point{left - thickness + 0.5F, bottom - inner_radius - 0.5F},
                    Point{left + 0.5F, bottom - inner_radius - 0.5F},
                    Point{left + 0.5F, top + inner_radius + 0.5F}}});
  draw_quads(quads, color);
}

class Scissor final {
public:
  explicit Scissor(Rect clip) {
    rlDrawRenderBatchActive();
    rlEnableScissorTest();
    rlScissor(clip.x, rlGetFramebufferHeight() - clip.y - clip.height, clip.width, clip.height);
  }
  Scissor(const Scissor &) = delete;
  Scissor &operator=(const Scissor &) = delete;
  ~Scissor() {
    rlDrawRenderBatchActive();
    rlDisableScissorTest();
  }
};

void begin_separate_blend() {
  rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA, RL_ONE, RL_ONE_MINUS_SRC_ALPHA,
                            RL_FUNC_ADD, RL_FUNC_ADD);
  rlSetBlendMode(RL_BLEND_CUSTOM_SEPARATE);
}

void end_separate_blend() { rlSetBlendMode(RL_BLEND_ALPHA); }

void ring_vertex(Point center, float angle, float radius, Rgba color, std::uint8_t alpha) {
  vertex({center.x + std::cos(angle * pi / 180.0F) * radius,
          center.y + std::sin(angle * pi / 180.0F) * radius},
         modulate_alpha(color, alpha));
}

void draw_antialiased_ring(Point center, float inner_radius, float outer_radius, float start_angle,
                           float end_angle, Rgba color) {
  constexpr float feather = 1.0F;
  const int segments = ring_segment_count(start_angle, end_angle);
  const float step = (end_angle - start_angle) / static_cast<float>(segments);
  constexpr std::array<std::uint8_t, 4> alphas{0, 255, 255, 0};
  const std::array radii{inner_radius - feather, inner_radius, outer_radius,
                         outer_radius + feather};
  rlSetTexture(0);
  rlBegin(RL_QUADS);
  for (int segment = 0; segment < segments; ++segment) {
    const float angle = start_angle + static_cast<float>(segment) * step;
    const float next = angle + step;
    for (std::size_t band = 0; band + 1U < radii.size(); ++band) {
      ring_vertex(center, angle, radii[band + 1U], color, alphas[band + 1U]);
      ring_vertex(center, angle, radii[band], color, alphas[band]);
      ring_vertex(center, next, radii[band], color, alphas[band]);
      ring_vertex(center, next, radii[band + 1U], color, alphas[band + 1U]);
    }
  }
  rlEnd();
}

void draw_disc_core(Point center, float radius, Rgba color) {
  constexpr int segments = 36;
  constexpr float step = 360.0F / static_cast<float>(segments);
  rlSetTexture(rlGetTextureIdDefault());
  rlBegin(RL_QUADS);
  for (int segment = 0; segment < segments / 2; ++segment) {
    const float angle = static_cast<float>(segment * 2) * step;
    ring_vertex(center, angle, 0.0F, color, 255);
    ring_vertex(center, angle + step * 2.0F, radius, color, 255);
    ring_vertex(center, angle + step, radius, color, 255);
    ring_vertex(center, angle, radius, color, 255);
  }
  rlEnd();
  rlSetTexture(0);
}

void draw_antialiased_disc(Point center, float radius, Rgba color) {
  constexpr float feather = 1.0F;
  constexpr int segments = 24;
  const float core_radius = std::max(0.0F, radius - feather);
  draw_disc_core(center, core_radius, color);
  const float step = 360.0F / static_cast<float>(segments);
  rlBegin(RL_QUADS);
  for (int segment = 0; segment < segments; ++segment) {
    const float angle = static_cast<float>(segment) * step;
    const float next = angle + step;
    ring_vertex(center, angle, radius, color, 0);
    ring_vertex(center, angle, core_radius, color, 255);
    ring_vertex(center, next, core_radius, color, 255);
    ring_vertex(center, next, radius, color, 0);
  }
  rlEnd();
}

void draw_antialiased_cap(Point center, float radius, Rgba color) {
  constexpr float feather = 1.0F;
  constexpr int segments = 24;
  draw_disc_core(center, radius, color);
  const float step = 360.0F / static_cast<float>(segments);
  rlBegin(RL_QUADS);
  for (int segment = 0; segment < segments; ++segment) {
    const float angle = static_cast<float>(segment) * step;
    const float next = angle + step;
    ring_vertex(center, angle, radius + feather, color, 0);
    ring_vertex(center, angle, radius, color, 255);
    ring_vertex(center, next, radius, color, 255);
    ring_vertex(center, next, radius + feather, color, 0);
  }
  rlEnd();
}

[[nodiscard]] std::expected<void, RlglPaintError> draw_shadow(const RoundedView &view,
                                                              RenderOrigin origin) {
  if (view.shadow.color.alpha == 0) {
    return {};
  }
  const int layers = std::max(1, view.shadow.blur + 1);
  Rgba color = view.shadow.color;
  color.alpha =
      static_cast<std::uint8_t>(std::max(1, static_cast<int>(view.shadow.color.alpha) / layers));
  begin_separate_blend();
  for (int blur = view.shadow.blur; blur >= 0; --blur) {
    const auto expansion = static_cast<std::int64_t>(view.shadow.spread) + blur;
    auto bounds =
        checked_rect(static_cast<std::int64_t>(view.bounds.x) + view.shadow.offset_x - expansion,
                     static_cast<std::int64_t>(view.bounds.y) + view.shadow.offset_y - expansion,
                     static_cast<std::int64_t>(view.bounds.width) + 2 * expansion,
                     static_cast<std::int64_t>(view.bounds.height) + 2 * expansion);
    if (!bounds) {
      end_separate_blend();
      return std::unexpected(bounds.error());
    }
    auto rendered = translated(*bounds, origin);
    if (!rendered) {
      end_separate_blend();
      return std::unexpected(rendered.error());
    }
    auto drawn = draw_rounded(
        *rendered, static_cast<float>(view.radius) + static_cast<float>(expansion), color);
    if (!drawn) {
      end_separate_blend();
      return drawn;
    }
  }
  end_separate_blend();
  return {};
}

} // namespace

std::expected<void, RlglPaintError> RlglPainter::draw_surface(const LayoutPlan &plan,
                                                              RenderOrigin origin) const {
  if (!session_.current()) {
    return std::unexpected(RlglPaintError::invalid_context);
  }
  auto bounds = translated(plan.view.bounds, origin);
  if (!bounds) {
    return std::unexpected(bounds.error());
  }
  auto shadow = draw_shadow(plan.view, origin);
  if (!shadow) {
    return shadow;
  }
  auto surface = draw_rounded(*bounds, static_cast<float>(plan.view.radius), plan.view.surface);
  if (!surface) {
    return surface;
  }
  if (plan.view.border > 0) {
    draw_rounded_border(*bounds, static_cast<float>(plan.view.radius),
                        static_cast<float>(plan.view.border), plan.view.border_color);
  }
  return {};
}

std::expected<void, RlglPaintError> RlglPainter::draw_content(const LayoutPlan &plan,
                                                              RenderOrigin origin) const {
  if (!session_.current()) {
    return std::unexpected(RlglPaintError::invalid_context);
  }
  for (const auto &content : plan.content) {
    auto drawn = std::visit(
        [this, origin](const auto &command) -> std::expected<void, RlglPaintError> {
          using Command = std::decay_t<decltype(command)>;
          if (command.clip.width <= 0 || command.clip.height <= 0) {
            return {};
          }
          auto clip = translated(command.clip, origin);
          auto bounds = translated(command.bounds, origin);
          if (!clip || !bounds) {
            return std::unexpected(RlglPaintError::invalid_geometry);
          }
          const Scissor scissor{*clip};
          if constexpr (std::is_same_v<Command, TextDrawCommand>) {
            if (fonts_ == nullptr) {
              return std::unexpected(RlglPaintError::unsupported_command);
            }
            const auto *font = fonts_->find(command.font_resource, command.typography);
            if (font == nullptr) {
              return std::unexpected(RlglPaintError::unsupported_command);
            }
            return draw_text(*font, command.text, static_cast<float>(bounds->x),
                             static_cast<float>(bounds->y),
                             static_cast<float>(command.typography.size), command.color);
          } else if constexpr (std::is_same_v<Command, IconDrawCommand>) {
            if (fonts_ == nullptr) {
              return std::unexpected(RlglPaintError::unsupported_command);
            }
            const auto *font = fonts_->find(command.font_resource, command.typography);
            if (font == nullptr) {
              return std::unexpected(RlglPaintError::unsupported_command);
            }
            return draw_codepoint(*font, command.codepoint, static_cast<float>(bounds->x),
                                  static_cast<float>(bounds->y),
                                  static_cast<float>(command.typography.size), command.color);
          } else if constexpr (std::is_same_v<Command, ImageDrawCommand>) {
            if (images_ == nullptr) {
              return std::unexpected(RlglPaintError::unsupported_command);
            }
            const auto *texture = images_->find(command);
            if (texture == nullptr) {
              return std::unexpected(RlglPaintError::unsupported_command);
            }
            draw_texture(*texture, *bounds);
            return {};
          } else if constexpr (std::is_same_v<Command, RichTextDrawCommand>) {
            if (rich_text_ == nullptr) {
              return std::unexpected(RlglPaintError::unsupported_command);
            }
            const auto *texture = rich_text_->find(command);
            if (texture == nullptr) {
              return std::unexpected(RlglPaintError::unsupported_command);
            }
            draw_texture(*texture, *bounds);
            return {};
          } else if constexpr (std::is_same_v<Command, ProgressDrawCommand>) {
            auto track = translated(command.track, origin);
            auto fill = translated(command.fill, origin);
            if (!track || !fill) {
              return std::unexpected(RlglPaintError::invalid_geometry);
            }
            auto track_drawn =
                draw_rounded(*track, static_cast<float>(track->height) / 2.0F, command.track_color);
            if (!track_drawn) {
              return track_drawn;
            }
            if (fill->width > 0 && fill->height > 0) {
              return draw_rounded(*fill, static_cast<float>(fill->height) / 2.0F,
                                  command.fill_color);
            }
            return {};
          } else if constexpr (std::is_same_v<Command, RingProgressDrawCommand>) {
            const Point center{
                static_cast<float>(bounds->x) + static_cast<float>(bounds->width) / 2.0F,
                static_cast<float>(bounds->y) + static_cast<float>(bounds->height) / 2.0F};
            const float outer = static_cast<float>(std::min(bounds->width, bounds->height)) / 2.0F;
            const float thickness = static_cast<float>(command.thickness);
            const float inner = outer - thickness;
            begin_separate_blend();
            draw_antialiased_ring(center, inner, outer, -90.0F, 270.0F, command.track_color);
            if (command.value > 0.0) {
              const float end = -90.0F + static_cast<float>(command.value * 360.0);
              draw_antialiased_ring(center, inner, outer, -90.0F, end, command.fill_color);
              const float middle = inner + thickness / 2.0F;
              const auto cap = [center, middle](float angle) {
                return Point{center.x + std::cos(angle * pi / 180.0F) * middle,
                             center.y + std::sin(angle * pi / 180.0F) * middle};
              };
              draw_antialiased_cap(cap(-90.0F), thickness / 2.0F, command.fill_color);
              draw_antialiased_cap(cap(end), thickness / 2.0F, command.fill_color);
            }
            end_separate_blend();
            return {};
          } else if constexpr (std::is_same_v<Command, IndicatorDrawCommand>) {
            const Point center{
                static_cast<float>(bounds->x) + static_cast<float>(bounds->width) / 2.0F,
                static_cast<float>(bounds->y) + static_cast<float>(bounds->height) / 2.0F};
            const float radius = static_cast<float>(std::min(bounds->width, bounds->height)) / 2.0F;
            begin_separate_blend();
            draw_antialiased_disc(center, radius, command.color);
            end_separate_blend();
            return {};
          } else if constexpr (std::is_same_v<Command, ButtonDecorationDrawCommand>) {
            return draw_rounded(*bounds, static_cast<float>(bounds->height) / 2.0F, command.color);
          } else {
            return std::unexpected(RlglPaintError::unsupported_command);
          }
        },
        content);
    if (!drawn) {
      return drawn;
    }
  }
  return {};
}

} // namespace gisland
