#include "gisland/rlgl_renderer_model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <variant>

namespace gisland {
namespace {

constexpr float pi = 3.14159265358979323846F;
constexpr int rounded_corner_segments = 16;

void append_rectangle(std::vector<Quad> &quads, float left, float top, float right, float bottom) {
  quads.push_back(Quad{std::array<Point, 4>{Point{left, top}, Point{left, bottom},
                                            Point{right, bottom}, Point{right, top}}});
}

} // namespace

Rect intersect_clip(Rect left, Rect right) noexcept {
  const auto left_edge = std::max<std::int64_t>(left.x, right.x);
  const auto top_edge = std::max<std::int64_t>(left.y, right.y);
  const auto right_edge = std::min(static_cast<std::int64_t>(left.x) + left.width,
                                   static_cast<std::int64_t>(right.x) + right.width);
  const auto bottom_edge = std::min(static_cast<std::int64_t>(left.y) + left.height,
                                    static_cast<std::int64_t>(right.y) + right.height);
  return Rect{static_cast<int>(left_edge), static_cast<int>(top_edge),
              static_cast<int>(std::max<std::int64_t>(0, right_edge - left_edge)),
              static_cast<int>(std::max<std::int64_t>(0, bottom_edge - top_edge))};
}

std::expected<Rect, RlglModelError> opengl_scissor(Rect clip, int framebuffer_height) noexcept {
  const auto bottom = static_cast<std::int64_t>(clip.y) + clip.height;
  if (clip.x < 0 || clip.y < 0 || clip.width < 0 || clip.height < 0 || framebuffer_height < 0 ||
      bottom > framebuffer_height) {
    return std::unexpected(RlglModelError::invalid_geometry);
  }
  return Rect{clip.x, framebuffer_height - static_cast<int>(bottom), clip.width, clip.height};
}

std::expected<ImageMapping, RlglModelError> map_image(Rect source, Rect destination,
                                                      ImageFit fit) noexcept {
  if (source.width <= 0 || source.height <= 0 || destination.width <= 0 ||
      destination.height <= 0) {
    return std::unexpected(RlglModelError::invalid_geometry);
  }

  const float scale_x = static_cast<float>(destination.width) / static_cast<float>(source.width);
  const float scale_y = static_cast<float>(destination.height) / static_cast<float>(source.height);
  const float scale =
      fit == ImageFit::cover ? std::max(scale_x, scale_y) : std::min(scale_x, scale_y);
  const float rendered_width = static_cast<float>(source.width) * scale;
  const float rendered_height = static_cast<float>(source.height) * scale;

  if (fit == ImageFit::contain) {
    return ImageMapping{{static_cast<float>(destination.x) +
                             (static_cast<float>(destination.width) - rendered_width) / 2.0F,
                         static_cast<float>(destination.y) +
                             (static_cast<float>(destination.height) - rendered_height) / 2.0F,
                         rendered_width, rendered_height},
                        {0.0F, 0.0F, 1.0F, 1.0F}};
  }

  const float visible_width = static_cast<float>(destination.width) / rendered_width;
  const float visible_height = static_cast<float>(destination.height) / rendered_height;
  return ImageMapping{{static_cast<float>(destination.x), static_cast<float>(destination.y),
                       static_cast<float>(destination.width),
                       static_cast<float>(destination.height)},
                      {(1.0F - visible_width) / 2.0F, (1.0F - visible_height) / 2.0F, visible_width,
                       visible_height}};
}

std::expected<RoundedRectangleMesh, RlglModelError> tessellate_rounded_rectangle(Rect bounds,
                                                                                 float radius) {
  if (bounds.width <= 0 || bounds.height <= 0 || !std::isfinite(radius)) {
    return std::unexpected(RlglModelError::invalid_geometry);
  }

  const float bounded_radius =
      std::clamp(radius, 0.0F, static_cast<float>(std::min(bounds.width, bounds.height)) / 2.0F);
  RoundedRectangleMesh mesh{{}, bounded_radius, rounded_corner_segments};
  mesh.quads.reserve(4U * static_cast<std::size_t>(rounded_corner_segments / 2) + 5U);

  const float left = static_cast<float>(bounds.x);
  const float top = static_cast<float>(bounds.y);
  const float right = left + static_cast<float>(bounds.width);
  const float bottom = top + static_cast<float>(bounds.height);
  if (bounded_radius == 0.0F) {
    append_rectangle(mesh.quads, left, top, right, bottom);
    return mesh;
  }

  const std::array<Point, 4> centers{{{left + bounded_radius, top + bounded_radius},
                                      {right - bounded_radius, top + bounded_radius},
                                      {right - bounded_radius, bottom - bounded_radius},
                                      {left + bounded_radius, bottom - bounded_radius}}};
  constexpr std::array<float, 4> start_angles{180.0F, 270.0F, 0.0F, 90.0F};
  constexpr float step = 90.0F / static_cast<float>(rounded_corner_segments);

  for (std::size_t corner = 0; corner < centers.size(); ++corner) {
    float angle = start_angles[corner];
    for (int segment = 0; segment < rounded_corner_segments / 2; ++segment) {
      mesh.quads.push_back(Quad{std::array<Point, 4>{
          centers[corner],
          Point{centers[corner].x + std::cos((angle + step * 2.0F) * pi / 180.0F) * bounded_radius,
                centers[corner].y + std::sin((angle + step * 2.0F) * pi / 180.0F) * bounded_radius},
          Point{centers[corner].x + std::cos((angle + step) * pi / 180.0F) * bounded_radius,
                centers[corner].y + std::sin((angle + step) * pi / 180.0F) * bounded_radius},
          Point{centers[corner].x + std::cos(angle * pi / 180.0F) * bounded_radius,
                centers[corner].y + std::sin(angle * pi / 180.0F) * bounded_radius}}});
      angle += step * 2.0F;
    }
  }

  append_rectangle(mesh.quads, left + bounded_radius, top, right - bounded_radius,
                   top + bounded_radius);
  append_rectangle(mesh.quads, right - bounded_radius, top + bounded_radius, right,
                   bottom - bounded_radius);
  append_rectangle(mesh.quads, left + bounded_radius, bottom - bounded_radius,
                   right - bounded_radius, bottom);
  append_rectangle(mesh.quads, left, top + bounded_radius, left + bounded_radius,
                   bottom - bounded_radius);
  append_rectangle(mesh.quads, left + bounded_radius, top + bounded_radius, right - bounded_radius,
                   bottom - bounded_radius);
  return mesh;
}

int ring_segment_count(float start_angle, float end_angle) noexcept {
  if (!std::isfinite(start_angle) || !std::isfinite(end_angle)) {
    return 1;
  }
  return std::max(1, static_cast<int>(std::ceil((end_angle - start_angle) / 4.0F)));
}

std::array<std::uint8_t, 4> straight_rgba_bytes(Rgba color) noexcept {
  return {color.red, color.green, color.blue, color.alpha};
}

Rgba modulate_alpha(Rgba color, std::uint8_t alpha) noexcept {
  color.alpha = static_cast<std::uint8_t>((static_cast<unsigned int>(color.alpha) * alpha) / 255U);
  return color;
}

std::expected<void, RlglModelError> validate_command_support(const ContentDrawCommand &command,
                                                             RlglCommandSupport support) noexcept {
  return std::visit(
      [support](const auto &value) -> std::expected<void, RlglModelError> {
        using Command = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Command, ImageDrawCommand>) {
          if (!support.images) {
            return std::unexpected(RlglModelError::unsupported_command);
          }
        } else if constexpr (std::is_same_v<Command, RichTextDrawCommand>) {
          if (!support.rich_text) {
            return std::unexpected(RlglModelError::unsupported_command);
          }
        }
        return {};
      },
      command);
}

} // namespace gisland
