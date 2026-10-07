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

float apply_easing(float progress, Easing easing) {
  switch (easing) {
  case Easing::linear:
    return std::clamp(progress, 0.0F, 1.0F);
  case Easing::ease_in:
    return cubic_bezier(progress, 0.42F, 0.0F, 1.0F, 1.0F);
  case Easing::ease_out:
    return cubic_bezier(progress, 0.0F, 0.0F, 0.58F, 1.0F);
  case Easing::ease_in_out:
    return cubic_bezier(progress, 0.42F, 0.0F, 0.58F, 1.0F);
  }
  return std::clamp(progress, 0.0F, 1.0F);
}

float smoothstep(float value) {
  const float clamped = std::clamp(value, 0.0F, 1.0F);
  return clamped * clamped * (3.0F - (2.0F * clamped));
}

IslandPoint mix(IslandPoint from, IslandPoint to, float progress) {
  return {mix(from.x, to.x, progress), mix(from.y, to.y, progress)};
}

void append_line(std::vector<IslandPoint> &points, IslandPoint from, IslandPoint to) {
  constexpr int samples = 4;
  for (int index = 1; index <= samples; ++index) {
    points.push_back(mix(from, to, static_cast<float>(index) / samples));
  }
}

void append_quadratic(std::vector<IslandPoint> &points, IslandPoint from, IslandPoint control,
                      IslandPoint to) {
  constexpr int samples = 8;
  for (int index = 1; index <= samples; ++index) {
    const float progress = static_cast<float>(index) / samples;
    const float inverse = 1.0F - progress;
    points.push_back({(inverse * inverse * from.x) + (2.0F * inverse * progress * control.x) +
                          (progress * progress * to.x),
                      (inverse * inverse * from.y) + (2.0F * inverse * progress * control.y) +
                          (progress * progress * to.y)});
  }
}

void append_cubic(std::vector<IslandPoint> &points, IslandPoint from, IslandPoint control1,
                  IslandPoint control2, IslandPoint to) {
  constexpr int samples = 12;
  for (int index = 1; index <= samples; ++index) {
    const float progress = static_cast<float>(index) / samples;
    const float inverse = 1.0F - progress;
    const float inverse2 = inverse * inverse;
    const float progress2 = progress * progress;
    points.push_back({(inverse2 * inverse * from.x) + (3.0F * inverse2 * progress * control1.x) +
                          (3.0F * inverse * progress2 * control2.x) + (progress2 * progress * to.x),
                      (inverse2 * inverse * from.y) + (3.0F * inverse2 * progress * control1.y) +
                          (3.0F * inverse * progress2 * control2.y) +
                          (progress2 * progress * to.y)});
  }
}

std::vector<IslandMaskRow> scanline_rows(const std::vector<IslandPoint> &contour, float center_x) {
  if (contour.empty()) {
    return {};
  }
  const auto [minimum_y, maximum_y] = std::ranges::minmax(contour, {}, &IslandPoint::y);
  const int first_y = std::max(0, static_cast<int>(std::floor(minimum_y.y)));
  const int last_y = static_cast<int>(std::ceil(maximum_y.y));
  std::vector<IslandMaskRow> rows;
  rows.reserve(static_cast<std::size_t>(std::max(0, last_y - first_y)));
  for (int y = first_y; y < last_y; ++y) {
    const float sample_y = static_cast<float>(y) + 0.5F;
    std::vector<float> intersections;
    intersections.reserve(contour.size());
    for (std::size_t index = 0; index < contour.size(); ++index) {
      const IslandPoint a = contour[index];
      const IslandPoint b = contour[(index + 1U) % contour.size()];
      if ((a.y <= sample_y && b.y > sample_y) || (b.y <= sample_y && a.y > sample_y)) {
        const float fraction = (sample_y - a.y) / (b.y - a.y);
        intersections.push_back(center_x + mix(a.x, b.x, fraction));
      }
    }
    if (intersections.size() < 2U) {
      continue;
    }
    const auto [left, right] = std::ranges::minmax(intersections);
    const int x = static_cast<int>(std::floor(left));
    const int end = static_cast<int>(std::ceil(right));
    if (end > x) {
      rows.push_back({.x = x, .y = y, .width = end - x, .height = 1});
    }
  }
  return rows;
}

} // namespace

void PresentationController::set_hidden_mode(bool hidden_mode, MonotonicTime now,
                                             bool active_interaction) {
  if (hidden_mode_ == hidden_mode) {
    return;
  }
  hidden_mode_ = hidden_mode;
  if (hidden_mode_ && (active_interaction || held_)) {
    consult(now);
    if (active_interaction) {
      set_interaction_hold(true, now);
    }
  }
}

void PresentationController::consult(MonotonicTime now) {
  consultation_ = true;
  consultation_deadline_ = held_ ? std::nullopt : std::optional{now + std::chrono::seconds{3}};
}

void PresentationController::reveal(ContextKey key, std::uint64_t revision,
                                    std::optional<MonotonicTime> expires_at) {
  reveal_source_ = RevealSource{std::move(key), revision, expires_at};
}

void PresentationController::reconcile(const std::optional<ContextKey> &key,
                                       std::uint64_t revision) {
  if (reveal_source_ &&
      (!key || reveal_source_->key != *key || reveal_source_->revision != revision)) {
    reveal_source_.reset();
  }
}

void PresentationController::reconcile(const std::optional<ContextKey> &compact_key,
                                       std::uint64_t compact_revision,
                                       const std::optional<ContextKey> &expanded_key,
                                       std::uint64_t expanded_revision) {
  if (!reveal_source_) {
    return;
  }
  const bool selected_compact = compact_key && reveal_source_->key == *compact_key &&
                                reveal_source_->revision == compact_revision;
  const bool selected_expanded = expanded_key && reveal_source_->key == *expanded_key &&
                                 reveal_source_->revision == expanded_revision;
  if (!selected_compact && !selected_expanded) {
    reveal_source_.reset();
  }
}

void PresentationController::set_interaction_hold(bool held, MonotonicTime now) {
  if (held_ == held) {
    return;
  }
  held_ = held;
  if (held_) {
    if (hidden_mode_ && should_present()) {
      consultation_ = true;
    }
    if (consultation_) {
      consultation_deadline_.reset();
    }
  } else if (consultation_) {
    consultation_deadline_ = now + std::chrono::seconds{3};
  }
}

void PresentationController::update(MonotonicTime now, bool has_content) {
  has_content_ = has_content;
  if (!has_content_) {
    held_ = false;
    consultation_ = false;
    consultation_deadline_.reset();
    reveal_source_.reset();
    return;
  }
  if (consultation_deadline_ && now >= *consultation_deadline_) {
    consultation_ = false;
    consultation_deadline_.reset();
  }
  if (reveal_source_ && reveal_source_->expires_at && now >= *reveal_source_->expires_at) {
    reveal_source_.reset();
  }
}

void PresentationController::close() {
  held_ = false;
  consultation_ = false;
  consultation_deadline_.reset();
  reveal_source_.reset();
}

void PresentationController::set_visible(bool visible) noexcept { visible_ = visible; }

bool PresentationController::hidden_mode() const noexcept { return hidden_mode_; }

bool PresentationController::should_present() const noexcept {
  return has_content_ &&
         (!hidden_mode_ || consultation_ || (reveal_source_.has_value() && has_content_));
}

bool PresentationController::visible() const noexcept { return visible_; }

IslandGeometry geometry_for(IslandMode mode) {
  if (mode == IslandMode::expanded) {
    return {.width = 420.0F, .height = 220.0F, .radius = 24.0F};
  }
  return {.width = 220.0F, .height = 44.0F, .radius = 22.0F};
}

IslandCanvasSize island_canvas_size() { return {.width = 440.0F, .height = 232.0F}; }

IslandCanvasSize fixed_canvas_for(const Theme &theme) {
  const auto rounded = [](double value) { return static_cast<int>(std::lround(value)); };
  const auto &views = theme.views();
  const auto &shadow = theme.shadow();
  double maximum_compact_width = views.compact.max_width;
  double maximum_compact_height =
      std::max(views.compact.max_height, theme.progress().compact_height);
  for (const auto &[name, style] : views.compact_styles) {
    (void)name;
    maximum_compact_width = std::max(maximum_compact_width, style.max_width);
    maximum_compact_height = std::max(maximum_compact_height, style.max_height);
  }
  double maximum_expanded_width = views.expanded.max_width;
  double maximum_expanded_height = views.expanded.max_height;
  for (const auto &[name, style] : views.expanded_styles) {
    (void)name;
    maximum_expanded_width = std::max(maximum_expanded_width, style.max_width);
    maximum_expanded_height = std::max(maximum_expanded_height, style.max_height);
  }
  const int surface_width = rounded(std::max(maximum_compact_width, maximum_expanded_width));
  const int surface_height = rounded(std::max(maximum_compact_height, maximum_expanded_height));
  const int shadow_radius = rounded(shadow.blur) + rounded(shadow.spread);
  const int shadow_x = rounded(shadow.offset_x);
  const int shadow_y = rounded(shadow.offset_y);
  const bool has_shadow = resolve_theme_color(theme, shadow.color).alpha != 0;
  const int left = has_shadow ? std::max(0, shadow_radius - shadow_x) : 0;
  const int top = has_shadow ? std::max(0, shadow_radius - shadow_y) : 0;
  const int right = has_shadow ? std::max(0, shadow_radius + shadow_x) : 0;
  const int bottom = has_shadow ? std::max(0, shadow_radius + shadow_y) : 0;
  return {
      .width = static_cast<float>(left + surface_width + right),
      .height = static_cast<float>(top + surface_height + bottom),
      .surface_x = static_cast<float>(left),
      .surface_y = static_cast<float>(top),
      .surface_width = static_cast<float>(surface_width),
      .surface_height = static_cast<float>(surface_height),
  };
}

IslandPlacement place_at_top_center(const IslandGeometry &geometry,
                                    const IslandCanvasSize &canvas) {
  const float surface_width = canvas.surface_width > 0.0F ? canvas.surface_width : canvas.width;
  return {.x = canvas.surface_x + ((surface_width - geometry.width) / 2.0F), .y = canvas.surface_y};
}

IslandGeometry interpolate(const IslandGeometry &from, const IslandGeometry &to, float progress) {
  return {
      .width = mix(from.width, to.width, progress),
      .height = mix(from.height, to.height, progress),
      .radius = mix(from.radius, to.radius, progress),
  };
}

void SpringProgress::set_target(float target) { target_ = std::clamp(target, 0.0F, 1.0F); }

void SpringProgress::snap(float value) {
  value_ = std::clamp(value, 0.0F, 1.0F);
  target_ = value_;
  velocity_ = 0.0F;
}

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

void ProgressAnimator::retarget(const LayoutPlan &plan, std::chrono::milliseconds duration,
                                Easing easing, bool preserve_current) {
  const float duration_seconds = std::max(std::chrono::duration<float>{duration}.count(), 0.0F);
  std::vector<Track> next;

  const auto add_target = [&](std::string_view path, double target,
                              std::optional<double> transition_from) {
    const auto previous =
        preserve_current ? std::ranges::find(tracks_, path, &Track::path) : tracks_.end();
    if (previous != tracks_.end() && previous->target == target) {
      next.push_back(*previous);
      return;
    }

    const std::optional<double> source =
        previous != tracks_.end() ? std::optional<double>{previous->current} : transition_from;
    if (!source.has_value() || duration_seconds <= 0.0F || *source == target) {
      return;
    }
    next.push_back(
        Track{std::string{path}, *source, *source, target, 0.0F, duration_seconds, easing, true});
  };

  for (const auto &command : plan.content) {
    if (const auto *progress = std::get_if<ProgressDrawCommand>(&command)) {
      add_target(progress->path, progress->value, progress->transition_from);
    } else if (const auto *ring = std::get_if<RingProgressDrawCommand>(&command)) {
      add_target(ring->path, ring->value, ring->transition_from);
    }
  }
  tracks_ = std::move(next);
}

void ProgressAnimator::update(float delta_seconds) {
  for (auto &track : tracks_) {
    if (!track.active) {
      continue;
    }
    track.elapsed_seconds += std::max(delta_seconds, 0.0F);
    const float progress = std::clamp(track.elapsed_seconds / track.duration_seconds, 0.0F, 1.0F);
    const double eased = static_cast<double>(apply_easing(progress, track.easing));
    track.current = track.source + ((track.target - track.source) * eased);
    if (progress >= 1.0F) {
      track.current = track.target;
      track.active = false;
    }
  }
}

bool ProgressAnimator::active() const { return std::ranges::any_of(tracks_, &Track::active); }

LayoutPlan ProgressAnimator::apply(const LayoutPlan &plan) const {
  auto animated = plan;
  for (auto &command : animated.content) {
    if (auto *progress = std::get_if<ProgressDrawCommand>(&command)) {
      const auto track = std::ranges::find(tracks_, progress->path, &Track::path);
      if (track == tracks_.end()) {
        continue;
      }
      progress->fill.width = static_cast<int>(
          std::lround(static_cast<double>(progress->track.width) * track->current));
    } else if (auto *ring = std::get_if<RingProgressDrawCommand>(&command)) {
      const auto track = std::ranges::find(tracks_, ring->path, &Track::path);
      if (track != tracks_.end()) {
        ring->value = track->current;
      }
    }
  }
  return animated;
}

namespace {

struct ContentOpacity {
  float outgoing;
  float incoming;
};

ContentOpacity black_fade_opacity(float progress) {
  constexpr float fade_out_end = 0.38F;
  constexpr float fade_in_start = 0.48F;
  const float clamped = std::clamp(progress, 0.0F, 1.0F);
  if (clamped < fade_out_end) {
    return {.outgoing = 1.0F - apply_easing(clamped / fade_out_end, Easing::ease_in),
            .incoming = 0.0F};
  }
  if (clamped <= fade_in_start) {
    return {.outgoing = 0.0F, .incoming = 0.0F};
  }
  return {.outgoing = 0.0F,
          .incoming =
              apply_easing((clamped - fade_in_start) / (1.0F - fade_in_start), Easing::ease_out)};
}

} // namespace

void ContentCrossfade::set_mode(IslandMode mode, std::chrono::milliseconds duration) {
  if (mode == mode_) {
    return;
  }
  outgoing_mode_ = mode_;
  outgoing_start_opacity_ =
      outgoing_mode_ == IslandMode::compact ? compact_.opacity : expanded_.opacity;
  mode_ = mode;
  elapsed_seconds_ = 0.0F;
  duration_seconds_ = std::max(std::chrono::duration<float>{duration}.count(), 0.0F);
  compact_ = {0.0F, 0.0F, 1.0F};
  expanded_ = {0.0F, 0.0F, 1.0F};
  if (duration_seconds_ <= 0.0F) {
    (mode_ == IslandMode::compact ? compact_ : expanded_).opacity = 1.0F;
    return;
  }
  (outgoing_mode_ == IslandMode::compact ? compact_ : expanded_).opacity = outgoing_start_opacity_;
}

void ContentCrossfade::update(float delta_seconds) {
  if (duration_seconds_ <= 0.0F) {
    return;
  }
  elapsed_seconds_ = std::min(elapsed_seconds_ + std::max(delta_seconds, 0.0F), duration_seconds_);
  const auto opacity = black_fade_opacity(elapsed_seconds_ / duration_seconds_);
  compact_.opacity = 0.0F;
  expanded_.opacity = 0.0F;
  (outgoing_mode_ == IslandMode::compact ? compact_ : expanded_).opacity =
      outgoing_start_opacity_ * opacity.outgoing;
  (mode_ == IslandMode::compact ? compact_ : expanded_).opacity = opacity.incoming;
  if (elapsed_seconds_ >= duration_seconds_) {
    duration_seconds_ = 0.0F;
  }
}

ContentVisual ContentCrossfade::compact() const { return compact_; }

ContentVisual ContentCrossfade::expanded() const { return expanded_; }

void ContextTransition::start(IslandGeometry source, IslandGeometry target,
                              std::chrono::milliseconds duration, Easing easing,
                              ContentTransition content, float slide_distance) {
  source_ = source;
  target_ = target;
  elapsed_seconds_ = 0.0F;
  duration_seconds_ = std::max(std::chrono::duration<float>{duration}.count(), 0.0F);
  easing_ = easing;
  content_ = content;
  slide_distance_ = std::max(slide_distance, 0.0F);
  active_ = duration_seconds_ > 0.0F;
  linear_progress_ = active_ ? 0.0F : 1.0F;
  progress_ = linear_progress_;
}

ContextTransitionKind classify_context_transition(const std::optional<ContextKey> &current_compact,
                                                  const std::optional<ContextKey> &current_expanded,
                                                  const std::optional<ContextKey> &next_compact,
                                                  const std::optional<ContextKey> &next_expanded) {
  return current_compact == next_compact && current_expanded == next_expanded
             ? ContextTransitionKind::aligned_content_crossfade
             : ContextTransitionKind::full_crossfade;
}

float context_incoming_opacity(ContextTransitionKind kind, const ContextTransitionVisual &visual) {
  return kind == ContextTransitionKind::full_crossfade ? visual.incoming_opacity : 1.0F;
}

float context_outgoing_opacity(ContextTransitionKind kind, const ContextTransitionVisual &visual) {
  if (kind == ContextTransitionKind::layout_only) {
    return 0.0F;
  }
  return kind == ContextTransitionKind::aligned_content_crossfade ? visual.local_outgoing_opacity
                                                                  : visual.outgoing_opacity;
}

bool use_layout_only_transition(bool keyboard_session, bool expanded_updated,
                                const std::optional<ContextKey> &current_expanded,
                                const std::optional<ContextKey> &next_expanded,
                                std::optional<ContentTransition> requested_transition) {
  return keyboard_session && expanded_updated && current_expanded &&
         current_expanded == next_expanded &&
         (!requested_transition || *requested_transition == ContentTransition::crossfade);
}

bool preserve_compact_during_expanded_switch(IslandMode current_mode, IslandMode requested_mode,
                                             const std::optional<ContextKey> &current_compact,
                                             const std::optional<ContextKey> &current_expanded,
                                             const std::optional<ContextKey> &next_compact,
                                             const std::optional<ContextKey> &next_expanded) {
  return current_mode == IslandMode::compact && requested_mode == IslandMode::expanded &&
         current_compact != next_compact && current_expanded != next_expanded &&
         next_expanded.has_value();
}

void ContextTransition::update(float delta_seconds) {
  if (!active_) {
    return;
  }
  elapsed_seconds_ += std::max(delta_seconds, 0.0F);
  linear_progress_ = std::clamp(elapsed_seconds_ / duration_seconds_, 0.0F, 1.0F);
  progress_ = apply_easing(linear_progress_, easing_);
  if (linear_progress_ >= 1.0F) {
    progress_ = 1.0F;
    active_ = false;
  }
}

bool ContextTransition::active() const { return active_; }

ContextTransitionVisual ContextTransition::visual() const {
  const auto opacity = black_fade_opacity(linear_progress_);
  ContextTransitionVisual result{
      .geometry = interpolate(source_, target_, progress_),
      .outgoing_opacity = opacity.outgoing,
      .incoming_opacity = opacity.incoming,
      .local_outgoing_opacity = 1.0F - progress_,
      .outgoing_offset_x = 0.0F,
      .incoming_offset_x = 0.0F,
      .surface_progress = progress_,
  };
  if (content_ == ContentTransition::slide_left) {
    result.outgoing_opacity = 1.0F;
    result.incoming_opacity = 1.0F;
    result.local_outgoing_opacity = 1.0F;
    result.outgoing_offset_x = -slide_distance_ * progress_;
    result.incoming_offset_x = slide_distance_ * (1.0F - progress_);
  } else if (content_ == ContentTransition::slide_right) {
    result.outgoing_opacity = 1.0F;
    result.incoming_opacity = 1.0F;
    result.local_outgoing_opacity = 1.0F;
    result.outgoing_offset_x = slide_distance_ * progress_;
    result.incoming_offset_x = -slide_distance_ * (1.0F - progress_);
  }
  if (!active_) {
    result.outgoing_opacity = 0.0F;
    result.incoming_opacity = 1.0F;
    result.local_outgoing_opacity = 0.0F;
    result.outgoing_offset_x = 0.0F;
    result.incoming_offset_x = 0.0F;
  }
  return result;
}

HoverController::HoverController(std::chrono::milliseconds exit_tolerance)
    : exit_tolerance_seconds_(
          std::max(std::chrono::duration<float>{exit_tolerance}.count(), 0.0F)) {}

void HoverController::set_exit_tolerance(std::chrono::milliseconds exit_tolerance) {
  exit_tolerance_seconds_ = std::max(std::chrono::duration<float>{exit_tolerance}.count(), 0.0F);
}

void HoverController::update(bool hovered, float delta_seconds) {
  if (hovered) {
    mode_ = IslandMode::expanded;
    outside_elapsed_ = 0.0F;
    return;
  }

  if (mode_ == IslandMode::expanded) {
    outside_elapsed_ += std::max(delta_seconds, 0.0F);
    if (outside_elapsed_ >= exit_tolerance_seconds_) {
      collapse();
    }
  }
}

void HoverController::collapse() {
  mode_ = IslandMode::compact;
  outside_elapsed_ = 0.0F;
}

IslandMode HoverController::mode() const { return mode_; }

OverlayModeController::OverlayModeController(std::chrono::milliseconds exit_tolerance)
    : hover_(exit_tolerance) {}

void OverlayModeController::update(bool hovered, bool has_expanded, float delta_seconds) {
  hovered_ = hovered;
  if (!has_expanded) {
    explicit_open_ = false;
    hover_suppressed_ = false;
    preview_remaining_ = 0.0F;
    persistent_reveal_ = false;
    hover_.collapse();
    return;
  }
  const float elapsed = std::max(delta_seconds, 0.0F);
  if (preview_remaining_ <= elapsed) {
    preview_remaining_ = 0.0F;
  } else {
    preview_remaining_ -= elapsed;
  }
  if (hover_suppressed_) {
    hover_.collapse();
    if (!hovered) {
      hover_suppressed_ = false;
    }
    return;
  }
  hover_.update(hovered, delta_seconds);
}

void OverlayModeController::start_preview(bool has_expanded, std::chrono::milliseconds duration) {
  set_reveal(has_expanded, duration);
}

void OverlayModeController::set_reveal(bool has_expanded,
                                       std::optional<std::chrono::milliseconds> duration) {
  persistent_reveal_ = has_expanded && !duration.has_value();
  preview_remaining_ = has_expanded && duration
                           ? std::max(std::chrono::duration<float>{*duration}.count(), 0.0F)
                           : 0.0F;
}

std::expected<void, ModeControlError> OverlayModeController::open(bool has_expanded) {
  if (!has_expanded) {
    return std::unexpected(ModeControlError::unavailable_expanded);
  }
  explicit_open_ = true;
  hover_suppressed_ = false;
  preview_remaining_ = 0.0F;
  persistent_reveal_ = false;
  return {};
}

void OverlayModeController::close() {
  explicit_open_ = false;
  hover_suppressed_ = hovered_;
  preview_remaining_ = 0.0F;
  persistent_reveal_ = false;
  hover_.collapse();
}

std::expected<void, ModeControlError> OverlayModeController::toggle(bool has_expanded) {
  if (mode() == IslandMode::expanded) {
    close();
    return {};
  }
  return open(has_expanded);
}

IslandMode OverlayModeController::mode() const {
  return explicit_open_ || persistent_reveal_ || preview_remaining_ > 0.0F ? IslandMode::expanded
                                                                           : hover_.mode();
}

bool OverlayModeController::explicitly_open() const noexcept { return explicit_open_; }

void OverlayModeController::set_exit_tolerance(std::chrono::milliseconds exit_tolerance) {
  hover_.set_exit_tolerance(exit_tolerance);
}

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

IslandRevealSilhouette reveal_silhouette(const IslandGeometry &destination, float progress,
                                         float top_margin) {
  if (!std::isfinite(progress) || progress <= 0.0F || destination.width <= 0.0F ||
      destination.height <= 0.0F) {
    return {};
  }

  const float p = std::clamp(progress, 0.0F, 1.1F);
  const float shape = std::min(1.0F, p);
  const float bounce = std::max(1.0F, p);
  const float width = destination.width * std::pow(shape, 1.6F) * bounce;
  const float height = destination.height * std::pow(shape, 0.9F) * bounce;
  const float top = std::max(0.0F, top_margin) * shape;
  const float radius = std::min({destination.radius, width / 2.0F, height / 2.0F});
  const float left = -width / 2.0F;
  const float right = width / 2.0F;
  const float bottom = top + height;
  const float detach = smoothstep((shape - 0.48F) / 0.30F);
  const float tip = top * detach;
  const float neck = std::min(14.0F, width * 0.14F) * (1.0F - detach);
  const float shoulder = std::max(0.0F, width / 2.0F - radius);

  std::vector<IslandPoint> contour;
  contour.reserve(88U);
  contour.push_back({-neck, tip});
  append_cubic(contour, contour.back(), {-neck, top}, {-shoulder * 0.35F, top}, {-shoulder, top});
  append_quadratic(contour, contour.back(), {left, top}, {left, top + radius});
  append_line(contour, contour.back(), {left, bottom - radius});
  append_quadratic(contour, contour.back(), {left, bottom}, {left + radius, bottom});
  append_line(contour, contour.back(), {right - radius, bottom});
  append_quadratic(contour, contour.back(), {right, bottom}, {right, bottom - radius});
  append_line(contour, contour.back(), {right, top + radius});
  append_quadratic(contour, contour.back(), {right, top}, {shoulder, top});
  append_cubic(contour, contour.back(), {shoulder * 0.35F, top}, {neck, top}, {neck, tip});
  append_line(contour, contour.back(), contour.front());
  auto rows = scanline_rows(contour, destination.width / 2.0F);

  return {.contour = std::move(contour),
          .rows = std::move(rows),
          .bounds = {.width = width, .height = top + height, .radius = radius},
          .top = top,
          .content_opacity = reveal_content_opacity(p, false)};
}

float reveal_content_opacity(float progress, bool departing) {
  return departing ? 0.0F : smoothstep((progress - 0.78F) / 0.22F);
}

IslandCanvasSize reserve_reveal_margin(IslandCanvasSize canvas, float margin) {
  const float reserved = std::max(0.0F, margin);
  const float surface_width = canvas.surface_width > 0.0F ? canvas.surface_width : canvas.width;
  const float surface_height = canvas.surface_height > 0.0F ? canvas.surface_height : canvas.height;
  const float overshoot_x = std::ceil(surface_width * 0.02F);
  const float overshoot_y = std::ceil(surface_height * 0.04F);
  canvas.width += overshoot_x * 2.0F;
  canvas.surface_x += overshoot_x;
  canvas.height += reserved + overshoot_y;
  canvas.surface_y += reserved;
  return canvas;
}

} // namespace gisland
