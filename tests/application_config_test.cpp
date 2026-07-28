#include "gisland/application.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("application configuration has stable defaults") {
  const gisland::ApplicationConfig config;
  CHECK(config.width == 640);
  CHECK(config.height == 360);
  CHECK(config.title == "gisland");
  CHECK(config.target_fps == 60);
}
