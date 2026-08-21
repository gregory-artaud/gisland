#include "gisland/rlgl_renderer_model.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <variant>

namespace {

using Catch::Approx;

void check_rect(gisland::FloatRect actual, gisland::FloatRect expected) {
  CHECK(actual.x == Approx(expected.x));
  CHECK(actual.y == Approx(expected.y));
  CHECK(actual.width == Approx(expected.width));
  CHECK(actual.height == Approx(expected.height));
}

} // namespace

TEST_CASE("portable clips intersect and convert to OpenGL coordinates") {
  CHECK(gisland::intersect_clip({10, 20, 30, 40}, {25, 5, 20, 30}) ==
        gisland::Rect{25, 20, 15, 15});
  CHECK(gisland::intersect_clip({0, 0, 10, 10}, {10, 0, 5, 5}) == gisland::Rect{10, 0, 0, 5});

  const auto scissor = gisland::opengl_scissor({7, 11, 13, 17}, 80);
  REQUIRE(scissor.has_value());
  CHECK(*scissor == gisland::Rect{7, 52, 13, 17});

  const auto invalid = gisland::opengl_scissor({0, 70, 20, 20}, 80);
  REQUIRE_FALSE(invalid.has_value());
  CHECK(invalid.error() == gisland::RlglModelError::invalid_geometry);
}

TEST_CASE("portable image mapping centers contain and cover crops") {
  const auto contain =
      gisland::map_image({0, 0, 200, 100}, {10, 20, 100, 100}, gisland::ImageFit::contain);
  REQUIRE(contain.has_value());
  check_rect(contain->destination, {10.0F, 45.0F, 100.0F, 50.0F});
  check_rect(contain->uv, {0.0F, 0.0F, 1.0F, 1.0F});

  const auto cover =
      gisland::map_image({0, 0, 200, 100}, {10, 20, 100, 100}, gisland::ImageFit::cover);
  REQUIRE(cover.has_value());
  check_rect(cover->destination, {10.0F, 20.0F, 100.0F, 100.0F});
  check_rect(cover->uv, {0.25F, 0.0F, 0.5F, 1.0F});

  CHECK_FALSE(
      gisland::map_image({0, 0, 0, 10}, {0, 0, 10, 10}, gisland::ImageFit::contain).has_value());
}

TEST_CASE("portable rounded rectangle tessellation matches pinned corner segments") {
  const auto rounded = gisland::tessellate_rounded_rectangle({10, 20, 80, 40}, 12.0F);
  REQUIRE(rounded.has_value());
  CHECK(rounded->corner_segments == 16);
  CHECK(rounded->radius == Approx(12.0F));
  CHECK(rounded->quads.size() == 37U);
  CHECK(rounded->quads.front().vertices.front() == gisland::Point{22.0F, 32.0F});

  const auto clamped = gisland::tessellate_rounded_rectangle({0, 0, 20, 10}, 20.0F);
  REQUIRE(clamped.has_value());
  CHECK(clamped->radius == Approx(5.0F));

  const auto square = gisland::tessellate_rounded_rectangle({0, 0, 20, 10}, 0.0F);
  REQUIRE(square.has_value());
  CHECK(square->quads.size() == 1U);

  CHECK_FALSE(gisland::tessellate_rounded_rectangle({0, 0, 0, 10}, 2.0F).has_value());
}

TEST_CASE("portable rings use deterministic four degree segmentation") {
  CHECK(gisland::ring_segment_count(0.0F, 360.0F) == 90);
  CHECK(gisland::ring_segment_count(-90.0F, 153.0F) == 61);
  CHECK(gisland::ring_segment_count(12.0F, 12.0F) == 1);
  CHECK(gisland::ring_segment_count(20.0F, 10.0F) == 1);
}

TEST_CASE("portable colors remain straight RGBA and modulate alpha deterministically") {
  const gisland::Rgba source{128, 64, 32, 127};
  CHECK(gisland::straight_rgba_bytes(source) == std::array<std::uint8_t, 4>{128, 64, 32, 127});
  CHECK(gisland::modulate_alpha(source, 128) == gisland::Rgba{128, 64, 32, 63});
}

TEST_CASE("portable command support rejects unavailable resource paths") {
  const gisland::ImageDrawCommand image{{}, {}, "cover", {}, "Cover"};
  const gisland::RichTextDrawCommand rich{};
  const gisland::TextDrawCommand text{};

  CHECK(gisland::validate_command_support(gisland::ContentDrawCommand{text}, {}) ==
        std::expected<void, gisland::RlglModelError>{});
  CHECK(gisland::validate_command_support(gisland::ContentDrawCommand{image}, {}).error() ==
        gisland::RlglModelError::unsupported_command);
  CHECK(gisland::validate_command_support(gisland::ContentDrawCommand{rich}, {}).error() ==
        gisland::RlglModelError::unsupported_command);
  CHECK(gisland::validate_command_support(gisland::ContentDrawCommand{image}, {.images = true}) ==
        std::expected<void, gisland::RlglModelError>{});
}
