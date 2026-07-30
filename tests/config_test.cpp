#include "gisland/config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
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

TEST_CASE("module views parse into typed templates") {
  constexpr auto source = R"(
monitor = "primary"
theme = "default"
default_module = "clock"

[[modules]]
id = "clock"
command = ["python3", "clock.py"]

[modules.view.compact]
type = "row"
gap = "small"
children = [
  { type = "text", value = { bind = "time" }, role = "primary" },
  { type = "text", value = { bind = "date_short" }, role = "muted" }
]

[modules.view.expanded]
type = "column"
children = [
  { repeat = "weeks", as = "week", template = { type = "row", children = [
    { repeat = "week", as = "day", template = { type = "text", value = { bind = "day.label" }, role = { bind = "day.role" } } }
  ] } }
]
)";

  const auto result = gisland::parse_config(source, "views.toml");

  REQUIRE(result.has_value());
  REQUIRE(result->modules.size() == 1);
  REQUIRE(result->modules.front().view.has_value());
  CHECK(std::holds_alternative<gisland::TemplateRow>(
      result->modules.front().view->compact.value));
  REQUIRE(result->modules.front().view->expanded.has_value());
  CHECK(std::holds_alternative<gisland::TemplateColumn>(
      result->modules.front().view->expanded->value));
}

TEST_CASE("invalid module view bindings fail at the TOML boundary") {
  constexpr auto source = R"(
monitor = "primary"
theme = "default"
default_module = "clock"
[[modules]]
id = "clock"
command = ["clock"]
[modules.view.compact]
type = "text"
value = { bind = "day..label" }
role = "body"
)";

  const auto result = gisland::parse_config(source, "invalid-view.toml");

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().path == "modules[0].view.compact.value.bind");
  CHECK(result.error().line > 0);
  CHECK(result.error().column > 0);
}

TEST_CASE("module view templates reject unknown properties and root repeats") {
  const auto parse_view = [](std::string_view compact) {
    return gisland::parse_config(
        std::string{"monitor=\"primary\"\ntheme=\"default\"\ndefault_module=\"clock\"\n"
                    "[[modules]]\nid=\"clock\"\ncommand=[\"clock\"]\n"
                    "[modules.view.compact]\n"} +
            std::string{compact},
        "invalid-view.toml");
  };

  const auto unknown = parse_view("type=\"text\"\nvalue=\"x\"\nrole=\"body\"\ncolour=\"red\"\n");
  REQUIRE_FALSE(unknown.has_value());
  CHECK(unknown.error().path == "modules[0].view.compact.colour");

  const auto repeat = parse_view("repeat=\"items\"\nas=\"item\"\ntemplate={type=\"text\",value=\"x\",role=\"body\"}\n");
  REQUIRE_FALSE(repeat.has_value());
  CHECK(repeat.error().path == "modules[0].view.compact");
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

TEST_CASE("module supervision settings parse into bounded typed values") {
  using namespace std::chrono_literals;

  constexpr auto source = R"(
monitor = "primary"
theme = "organic"
default_module = "clock"

[[modules]]
id = "clock"
command = ["/usr/bin/clock", "--jsonl"]
restart = "always"
working_directory = "/var/lib/gisland-clock"

[modules.environment]
LANG = "en_GB.UTF-8"
GISLAND_TEST = "configured"

[modules.timings]
handshake_ms = 1500
graceful_shutdown_ms = 900
terminate_grace_ms = 400
initial_backoff_ms = 100
maximum_backoff_ms = 5000
healthy_reset_ms = 45000
)";

  const auto result = gisland::parse_config(source, "supervision.toml");

  REQUIRE(result.has_value());
  REQUIRE(result->modules.size() == 1);
  const auto &module = result->modules.front();
  CHECK(module.restart == gisland::RestartPolicy::always);
  CHECK(module.timings.handshake == 1500ms);
  CHECK(module.timings.graceful_shutdown == 900ms);
  CHECK(module.timings.terminate_grace == 400ms);
  CHECK(module.timings.initial_backoff == 100ms);
  CHECK(module.timings.maximum_backoff == 5000ms);
  CHECK(module.timings.healthy_reset == 45000ms);
  CHECK(module.environment.at("LANG") == "en_GB.UTF-8");
  CHECK(module.environment.at("GISLAND_TEST") == "configured");
  REQUIRE(module.working_directory.has_value());
  CHECK(module.working_directory.value_or(std::filesystem::path{}) ==
        std::filesystem::path{"/var/lib/gisland-clock"});
}

TEST_CASE("module supervision settings have documented defaults") {
  using namespace std::chrono_literals;

  constexpr auto source = R"(
monitor = "primary"
theme = "organic"
default_module = "clock"

[[modules]]
id = "clock"
command = ["clock"]
)";

  const auto result = gisland::parse_config(source, "defaults.toml");

  REQUIRE(result.has_value());
  const auto &module = result->modules.front();
  CHECK(module.restart == gisland::RestartPolicy::on_failure);
  CHECK(module.timings.handshake == 2s);
  CHECK(module.timings.graceful_shutdown == 1s);
  CHECK(module.timings.terminate_grace == 500ms);
  CHECK(module.timings.initial_backoff == 250ms);
  CHECK(module.timings.maximum_backoff == 30s);
  CHECK(module.timings.healthy_reset == 60s);
  CHECK(module.environment.empty());
  CHECK_FALSE(module.working_directory.has_value());
}

TEST_CASE("invalid module supervision settings are rejected at the TOML boundary") {
  SECTION("unknown restart policy") {
    constexpr auto source = R"(
monitor = "primary"
theme = "organic"
default_module = "clock"
[[modules]]
id = "clock"
command = ["clock"]
restart = "sometimes"
)";
    const auto result = gisland::parse_config(source, "restart.toml");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "modules[0].restart");
  }

  SECTION("negative duration") {
    constexpr auto source = R"(
monitor = "primary"
theme = "organic"
default_module = "clock"
[[modules]]
id = "clock"
command = ["clock"]
[modules.timings]
handshake_ms = -1
)";
    const auto result = gisland::parse_config(source, "duration.toml");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "modules[0].timings.handshake_ms");
  }

  SECTION("initial backoff above maximum") {
    constexpr auto source = R"(
monitor = "primary"
theme = "organic"
default_module = "clock"
[[modules]]
id = "clock"
command = ["clock"]
[modules.timings]
initial_backoff_ms = 2000
maximum_backoff_ms = 1000
)";
    const auto result = gisland::parse_config(source, "backoff.toml");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "modules[0].timings.initial_backoff_ms");
  }

  SECTION("working directory is relative") {
    constexpr auto source = R"(
monitor = "primary"
theme = "organic"
default_module = "clock"
[[modules]]
id = "clock"
command = ["clock"]
working_directory = "relative/path"
)";
    const auto result = gisland::parse_config(source, "cwd.toml");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "modules[0].working_directory");
  }
}
