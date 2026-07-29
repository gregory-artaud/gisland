#include "gisland/island.hpp"

#include <algorithm>
#include <cmath>

namespace gisland {
namespace {

float mix(float from, float to, float progress) { return from + ((to - from) * progress); }

} // namespace

IslandGeometry geometry_for(IslandMode mode) {
  if (mode == IslandMode::expanded) {
    return {.width = 420.0F, .height = 220.0F, .radius = 24.0F};
  }
  return {.width = 220.0F, .height = 44.0F, .radius = 22.0F};
}

IslandCanvasSize island_canvas_size() { return {.width = 440.0F, .height = 232.0F}; }

IslandPlacement place_at_top_center(const IslandGeometry &geometry,
                                    const IslandCanvasSize &canvas) {
  return {.x = (canvas.width - geometry.width) / 2.0F, .y = 0.0F};
}

IslandGeometry interpolate(const IslandGeometry &from, const IslandGeometry &to, float progress) {
  return {
      .width = mix(from.width, to.width, progress),
      .height = mix(from.height, to.height, progress),
      .radius = mix(from.radius, to.radius, progress),
  };
}

void SpringProgress::set_target(float target) { target_ = std::clamp(target, 0.0F, 1.0F); }

void SpringProgress::update(float delta_seconds) {
  constexpr float stiffness = 220.0F;
  constexpr float damping = 21.36F;
  constexpr float maximum_step = 1.0F / 240.0F;

  float remaining = std::clamp(delta_seconds, 0.0F, 0.05F);
  while (remaining > 0.0F) {
    const float step = std::min(remaining, maximum_step);
    const float acceleration = (stiffness * (target_ - value_)) - (damping * velocity_);
    velocity_ += acceleration * step;
    value_ += velocity_ * step;
    remaining -= step;
  }

  if (std::fabs(target_ - value_) < 0.0005F && std::fabs(velocity_) < 0.005F) {
    value_ = target_;
    velocity_ = 0.0F;
  }
}

float SpringProgress::value() const { return value_; }

void HoverController::update(bool hovered, float delta_seconds) {
  if (hovered) {
    mode_ = IslandMode::expanded;
    outside_elapsed_ = 0.0F;
    return;
  }

  if (mode_ == IslandMode::expanded) {
    outside_elapsed_ += std::max(delta_seconds, 0.0F);
    if (outside_elapsed_ >= 0.15F) {
      collapse();
    }
  }
}

void HoverController::collapse() {
  mode_ = IslandMode::compact;
  outside_elapsed_ = 0.0F;
}

IslandMode HoverController::mode() const { return mode_; }

std::vector<IslandMaskRow> rounded_mask_rows(const IslandGeometry &geometry) {
  const int width = std::max(1, static_cast<int>(std::lround(geometry.width)));
  const int height = std::max(1, static_cast<int>(std::lround(geometry.height)));
  const float radius = std::clamp(geometry.radius, 0.0F, static_cast<float>(height) / 2.0F);

  std::vector<IslandMaskRow> rows;
  rows.reserve(static_cast<std::size_t>(height));

  for (int y = 0; y < height; ++y) {
    float inset = 0.0F;
    const float pixel_center = static_cast<float>(y) + 0.5F;
    if (pixel_center < radius) {
      const float distance = radius - pixel_center;
      inset = radius - std::sqrt((radius * radius) - (distance * distance));
    } else if (pixel_center > static_cast<float>(height) - radius) {
      const float distance = pixel_center - (static_cast<float>(height) - radius);
      inset = radius - std::sqrt((radius * radius) - (distance * distance));
    }

    const int x = std::max(0, static_cast<int>(std::floor(inset)));
    rows.push_back({.x = x, .y = y, .width = std::max(1, width - (2 * x)), .height = 1});
  }

  return rows;
}

} // namespace gisland
