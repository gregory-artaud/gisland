#include "gisland/config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <variant>

TEST_CASE("minimum application configuration parses into typed values") {
  constexpr auto source = R"(
monitor = "DP-1"
theme = "organic"
default_module = "clock"

[[modules]]
id = "clock"
command = ["/usr/libexec/gisland-clock", "--jsonl"]
enabled = true

[modules.options]
timezone = "UTC"
show_date = true
refresh_ms = 1000
weekend_days = [6, 7]

[modules.options.labels]
today = "Today"

[[modules]]
id = "music"
command = ["/usr/bin/music-status"]
enabled = false
)";

  const auto result = gisland::parse_config(source, "config.toml");

  REQUIRE(result.has_value());
  CHECK(result->monitor == "DP-1");
  CHECK(result->theme == "organic");
  CHECK(result->default_module == "clock");
  REQUIRE(result->modules.size() == 2);

  const auto &clock = result->modules[0];
  CHECK(clock.id == "clock");
  CHECK((clock.command == std::vector<std::string>{"/usr/libexec/gisland-clock", "--jsonl"}));
  CHECK(clock.enabled);
  CHECK(std::get<std::string>(clock.options.at("timezone").value) == "UTC");
  CHECK(std::get<bool>(clock.options.at("show_date").value));
  CHECK(std::get<std::int64_t>(clock.options.at("refresh_ms").value) == 1000);
  const auto &weekend_days =
      std::get<gisland::ConfigValue::Array>(clock.options.at("weekend_days").value);
  REQUIRE(weekend_days.size() == 2);
  CHECK(std::get<std::int64_t>(weekend_days[0].value) == 6);
  const auto &labels = std::get<gisland::ConfigValue::Table>(clock.options.at("labels").value);
  CHECK(std::get<std::string>(labels.at("today").value) == "Today");

  const auto &music = result->modules[1];
  CHECK(music.id == "music");
  CHECK_FALSE(music.enabled);
  CHECK(music.options.empty());
}

TEST_CASE("default module must reference an enabled instance") {
  constexpr auto missing_default = R"(
monitor = "primary"
theme = "organic"
default_module = "missing"

[[modules]]
id = "clock"
command = ["clock"]
)";
  auto result = gisland::parse_config(missing_default, "missing.toml");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().path == "default_module");

  constexpr auto disabled_default = R"(
monitor = "primary"
theme = "organic"
default_module = "clock"

[[modules]]
id = "clock"
command = ["clock"]
enabled = false
)";
  result = gisland::parse_config(disabled_default, "disabled.toml");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().path == "default_module");
}

TEST_CASE("module IDs are non-empty and unique") {
  constexpr auto duplicate = R"(
monitor = "primary"
theme = "organic"
default_module = "clock"

[[modules]]
id = "clock"
command = ["clock"]

[[modules]]
id = "clock"
command = ["other"]
)";
  const auto duplicate_result = gisland::parse_config(duplicate, "duplicate.toml");
  REQUIRE_FALSE(duplicate_result.has_value());
  CHECK(duplicate_result.error().path == "modules[1].id");

  constexpr auto empty = R"(
monitor = "primary"
theme = "organic"
default_module = "clock"

[[modules]]
id = ""
command = ["clock"]
)";
  const auto empty_result = gisland::parse_config(empty, "empty.toml");
  REQUIRE_FALSE(empty_result.has_value());
  CHECK(empty_result.error().path == "modules[0].id");
}

TEST_CASE("module command requires a non-empty executable") {
  constexpr auto source = R"(
monitor = "primary"
theme = "organic"
default_module = "clock"

[[modules]]
id = "clock"
command = [""]
)";

  const auto result = gisland::parse_config(source, "command.toml");

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().path == "modules[0].command[0]");
}

TEST_CASE("TOML parse errors preserve source position") {
  const auto result = gisland::parse_config("monitor = [", "broken.toml");

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().source == "broken.toml");
  CHECK(result.error().line == 1);
  CHECK(result.error().column > 0);
  CHECK_FALSE(result.error().message.empty());
}

TEST_CASE("unsupported TOML option values are rejected at the boundary") {
  constexpr auto source = R"(
monitor = "primary"
theme = "organic"
default_module = "clock"

[[modules]]
id = "clock"
command = ["clock"]

[modules.options]
day = 2026-07-28
)";

  const auto result = gisland::parse_config(source, "date.toml");

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().path == "modules[0].options.day");
}
