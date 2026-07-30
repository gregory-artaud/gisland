#include "gisland/island.hpp"

#include <algorithm>
#include <cmath>

namespace gisland {
namespace {

float mix(float from, float to, float progress) { return from + ((to - from) * progress); }

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
float cubic_bezier(float progress, float x1, float y1, float x2, float y2) {
  const auto sample = [](float point, float control1, float control2) {
    const float inverse = 1.0F - point;
    return (3.0F * inverse * inverse * point * control1) +
           (3.0F * inverse * point * point * control2) + (point * point * point);
  };
  const auto derivative = [](float point, float control1, float control2) {
    const float inverse = 1.0F - point;
    return (3.0F * inverse * inverse * control1) +
           (6.0F * inverse * point * (control2 - control1)) +
           (3.0F * point * point * (1.0F - control2));
  };

  const float clamped = std::clamp(progress, 0.0F, 1.0F);
  float point = clamped;
  for (int iteration = 0; iteration < 6; ++iteration) {
    const float slope = derivative(point, x1, x2);
    if (std::fabs(slope) < 0.0001F) {
      break;
    }
    point = std::clamp(point - ((sample(point, x1, x2) - clamped) / slope), 0.0F, 1.0F);
  }
  return sample(point, y1, y2);
}

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

void ContentCrossfade::set_mode(IslandMode mode) {
  if (mode == mode_) {
    return;
  }
  mode_ = mode;
  retarget(compact_, mode == IslandMode::compact);
  retarget(expanded_, mode == IslandMode::expanded);
}

void ContentCrossfade::update(float delta_seconds) {
  const float nonnegative_delta = std::max(delta_seconds, 0.0F);
  update_layer(compact_, nonnegative_delta);
  update_layer(expanded_, nonnegative_delta);
}

ContentVisual ContentCrossfade::compact() const { return compact_.value; }

ContentVisual ContentCrossfade::expanded() const { return expanded_.value; }

void ContentCrossfade::retarget(LayerTransition &layer, bool active) {
  layer.start = layer.value;
  layer.target = active ? ContentVisual{1.0F, 0.0F, 1.0F} : ContentVisual{0.0F, 6.0F, 0.96F};
  layer.elapsed = 0.0F;
  layer.delay = active ? 0.06F : 0.0F;
}

void ContentCrossfade::update_layer(LayerTransition &layer, float delta_seconds) {
  constexpr float opacity_duration = 0.25F;
  constexpr float blur_duration = 0.30F;
  constexpr float scale_duration = 0.35F;

  layer.elapsed += delta_seconds;
  const float active_elapsed = std::max(layer.elapsed - layer.delay, 0.0F);
  const float opacity_progress =
      cubic_bezier(active_elapsed / opacity_duration, 0.25F, 0.1F, 0.25F, 1.0F);
  const float blur_progress =
      cubic_bezier(active_elapsed / blur_duration, 0.25F, 0.1F, 0.25F, 1.0F);
  const float scale_progress =
      cubic_bezier(active_elapsed / scale_duration, 0.175F, 0.885F, 0.32F, 1.1F);
  layer.value = {
      .opacity = mix(layer.start.opacity, layer.target.opacity, opacity_progress),
      .blur = mix(layer.start.blur, layer.target.blur, blur_progress),
      .scale = mix(layer.start.scale, layer.target.scale, scale_progress),
  };
}

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
