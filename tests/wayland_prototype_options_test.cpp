#include "wayland_prototype_options.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

namespace {

using namespace std::string_view_literals;

template <std::size_t Size> auto parse(const std::array<std::string_view, Size> &arguments) {
  return gisland::wayland_prototype::parse_options(arguments);
}

} // namespace

TEST_CASE("Wayland prototype options default to manual mode and stable geometry") {
  const auto options = parse(std::array<std::string_view, 0>{});

  REQUIRE(options.has_value());
  CHECK_FALSE(options->automated);
  CHECK(options->width == 420);
  CHECK(options->height == 220);
  CHECK(options->top_margin == 8);
}

TEST_CASE("Wayland prototype options parse automated mode and bounded geometry") {
  const std::array arguments{"--automated"sv, "--width"sv,      "640"sv, "--height"sv,
                             "360"sv,         "--top-margin"sv, "24"sv};
  const auto options = parse(arguments);

  REQUIRE(options.has_value());
  CHECK(options->automated);
  CHECK(options->width == 640);
  CHECK(options->height == 360);
  CHECK(options->top_margin == 24);
}

TEST_CASE("Wayland prototype options reject unknown missing malformed and unbounded values") {
  CHECK_FALSE(parse(std::array{"--unknown"sv}).has_value());
  CHECK_FALSE(parse(std::array{"--width"sv}).has_value());
  CHECK_FALSE(parse(std::array{"--height"sv, "large"sv}).has_value());
  CHECK_FALSE(parse(std::array{"--width"sv, "0"sv}).has_value());
  CHECK_FALSE(parse(std::array{"--width"sv, "40"sv}).has_value());
  CHECK_FALSE(parse(std::array{"--height"sv, "8193"sv}).has_value());
  CHECK_FALSE(parse(std::array{"--top-margin"sv, "-1"sv}).has_value());
  CHECK_FALSE(parse(std::array{"--top-margin"sv, "4097"sv}).has_value());
}
