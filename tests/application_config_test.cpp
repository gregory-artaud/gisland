#include "gisland/application.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("application configuration has stable defaults") {
  const gisland::ApplicationConfig config;
  CHECK(config.title == "gisland");
  CHECK(config.target_fps == 60);
  CHECK(config.monitor == "primary");
  CHECK(config.top_margin == 8);
}
