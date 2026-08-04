#pragma once

#include <chrono>
#include <expected>
#include <vector>

namespace gisland {

enum class IslandMode { compact, expanded };

struct IslandGeometry {
  float width;
  float height;
  float radius;
};

struct IslandCanvasSize {
  float width;
  float height;
};

struct IslandPlacement {
  float x;
  float y;
};

struct IslandMaskRow {
  int x;
  int y;
  int width;
  int height;
};

class HoverController {
public:
  explicit HoverController(std::chrono::milliseconds exit_tolerance = std::chrono::milliseconds{
                               120});
  void update(bool hovered, float delta_seconds);
  void collapse();
  void set_exit_tolerance(std::chrono::milliseconds exit_tolerance);
  [[nodiscard]] IslandMode mode() const;

private:
  IslandMode mode_{IslandMode::compact};
  float outside_elapsed_{0.0F};
  float exit_tolerance_seconds_;
};

enum class ModeControlError { unavailable_expanded };

class OverlayModeController {
public:
  explicit OverlayModeController(
      std::chrono::milliseconds exit_tolerance = std::chrono::milliseconds{120});

  void update(bool hovered, bool has_expanded, float delta_seconds);
  [[nodiscard]] std::expected<void, ModeControlError> open(bool has_expanded);
  void close();
  [[nodiscard]] std::expected<void, ModeControlError> toggle(bool has_expanded);
  void set_exit_tolerance(std::chrono::milliseconds exit_tolerance);
  [[nodiscard]] IslandMode mode() const;

private:
  HoverController hover_;
  bool explicit_open_{false};
  bool hover_suppressed_{false};
  bool hovered_{false};
};

class SpringProgress {
public:
  void set_target(float target);
  void update(float delta_seconds);
  [[nodiscard]] float value() const;

private:
  float value_{0.0F};
  float velocity_{0.0F};
  float target_{0.0F};
};

struct ContentVisual {
  float opacity;
  float blur;
  float scale;
};

class ContentCrossfade {
public:
  void set_mode(IslandMode mode);
  void update(float delta_seconds);
  [[nodiscard]] ContentVisual compact() const;
  [[nodiscard]] ContentVisual expanded() const;

private:
  struct LayerTransition {
    ContentVisual value;
    ContentVisual start;
    ContentVisual target;
    float elapsed;
    float delay;
  };

  static void retarget(LayerTransition &layer, bool active);
  static void update_layer(LayerTransition &layer, float delta_seconds);

  IslandMode mode_{IslandMode::compact};
  LayerTransition compact_{{1.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 1.0F}, 0.0F, 0.0F};
  LayerTransition expanded_{
      {0.0F, 6.0F, 0.96F}, {0.0F, 6.0F, 0.96F}, {0.0F, 6.0F, 0.96F}, 0.0F, 0.0F};
};

[[nodiscard]] IslandGeometry geometry_for(IslandMode mode);
[[nodiscard]] IslandCanvasSize island_canvas_size();
[[nodiscard]] IslandPlacement place_at_top_center(const IslandGeometry &geometry,
                                                  const IslandCanvasSize &canvas);
[[nodiscard]] IslandGeometry interpolate(const IslandGeometry &from, const IslandGeometry &to,
                                         float progress);
[[nodiscard]] std::vector<IslandMaskRow> rounded_mask_rows(const IslandGeometry &geometry);

} // namespace gisland
