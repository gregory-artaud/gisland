#pragma once

#include "gisland/layout.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <vector>

namespace gisland {

enum class RlglModelError { invalid_geometry, unsupported_command };

struct Point {
  float x;
  float y;

  bool operator==(const Point &) const = default;
};

struct FloatRect {
  float x;
  float y;
  float width;
  float height;
};

struct Quad {
  std::array<Point, 4> vertices;

  bool operator==(const Quad &) const = default;
};

struct ImageMapping {
  FloatRect destination;
  FloatRect uv;
};

struct RoundedRectangleMesh {
  std::vector<Quad> quads;
  float radius;
  int corner_segments;
};

struct RlglCommandSupport {
  bool images{};
  bool rich_text{};
};

[[nodiscard]] Rect intersect_clip(Rect left, Rect right) noexcept;
[[nodiscard]] std::expected<Rect, RlglModelError> opengl_scissor(Rect clip,
                                                                 int framebuffer_height) noexcept;
[[nodiscard]] std::expected<ImageMapping, RlglModelError> map_image(Rect source, Rect destination,
                                                                    ImageFit fit) noexcept;
[[nodiscard]] std::expected<RoundedRectangleMesh, RlglModelError>
tessellate_rounded_rectangle(Rect bounds, float radius);
[[nodiscard]] int ring_segment_count(float start_angle, float end_angle) noexcept;
[[nodiscard]] std::array<std::uint8_t, 4> straight_rgba_bytes(Rgba color) noexcept;
[[nodiscard]] Rgba modulate_alpha(Rgba color, std::uint8_t alpha) noexcept;
[[nodiscard]] std::expected<void, RlglModelError>
validate_command_support(const ContentDrawCommand &command, RlglCommandSupport support) noexcept;

} // namespace gisland
