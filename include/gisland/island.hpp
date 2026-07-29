#pragma once

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
  void update(bool hovered, float delta_seconds);
  void collapse();
  [[nodiscard]] IslandMode mode() const;

private:
  IslandMode mode_{IslandMode::compact};
  float outside_elapsed_{0.0F};
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

[[nodiscard]] IslandGeometry geometry_for(IslandMode mode);
[[nodiscard]] IslandCanvasSize island_canvas_size();
[[nodiscard]] IslandPlacement place_at_top_center(const IslandGeometry &geometry,
                                                  const IslandCanvasSize &canvas);
[[nodiscard]] IslandGeometry interpolate(const IslandGeometry &from, const IslandGeometry &to,
                                         float progress);
[[nodiscard]] std::vector<IslandMaskRow> rounded_mask_rows(const IslandGeometry &geometry);

} // namespace gisland
