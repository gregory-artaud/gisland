#include "gisland/config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>

namespace {

[[nodiscard]] std::string config_with_interaction(std::string_view interaction) {
  return std::string{R"(monitor = "primary"
theme = "organic"
default_module = "clock"

[interaction]
)"} + std::string{interaction} +
         R"(

[[modules]]
id = "clock"
command = ["clock"]
)";
}

[[nodiscard]] std::string config_with_module_field(std::string_view field) {
  return std::string{R"(monitor = "primary"
theme = "organic"
default_module = "clock"

[[modules]]
id = "clock"
command = ["clock"]
)"} + std::string{field};
}

} // namespace

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
  CHECK(result->interaction.animation_speed == 1.25);
  CHECK(result->interaction.hover_exit == std::chrono::milliseconds{120});
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

TEST_CASE("interaction timing parses explicit values and accepted boundaries") {
  SECTION("explicit values") {
    const auto result = gisland::parse_config(
        config_with_interaction("animation_speed = 2.0\nhover_exit_ms = 450\n"),
        "interaction.toml");

    REQUIRE(result.has_value());
    CHECK(result->interaction.animation_speed == 2.0);
    CHECK(result->interaction.hover_exit == std::chrono::milliseconds{450});
  }

  SECTION("minimum values") {
    const auto result = gisland::parse_config(
        config_with_interaction("animation_speed = 0.25\nhover_exit_ms = 0\n"), "interaction.toml");

    REQUIRE(result.has_value());
    CHECK(result->interaction.animation_speed == 0.25);
    CHECK(result->interaction.hover_exit == std::chrono::milliseconds{0});
  }

  SECTION("maximum values") {
    const auto result = gisland::parse_config(
        config_with_interaction("animation_speed = 4.0\nhover_exit_ms = 2000\n"),
        "interaction.toml");

    REQUIRE(result.has_value());
    CHECK(result->interaction.animation_speed == 4.0);
    CHECK(result->interaction.hover_exit == std::chrono::milliseconds{2000});
  }
}

TEST_CASE("invalid interaction timing is rejected at the TOML boundary") {
  const auto check_error_path = [](std::string_view interaction, std::string_view expected_path) {
    const auto result =
        gisland::parse_config(config_with_interaction(interaction), "interaction.toml");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == expected_path);
  };

  SECTION("animation speed type") {
    check_error_path("animation_speed = \"fast\"\n", "interaction.animation_speed");
  }
  SECTION("hover exit type") {
    check_error_path("hover_exit_ms = 120.0\n", "interaction.hover_exit_ms");
  }
  SECTION("non-finite animation speed") {
    check_error_path("animation_speed = inf\n", "interaction.animation_speed");
  }
  SECTION("animation speed below minimum") {
    check_error_path("animation_speed = 0.24\n", "interaction.animation_speed");
  }
  SECTION("animation speed above maximum") {
    check_error_path("animation_speed = 4.01\n", "interaction.animation_speed");
  }
  SECTION("hover exit below minimum") {
    check_error_path("hover_exit_ms = -1\n", "interaction.hover_exit_ms");
  }
  SECTION("hover exit above maximum") {
    check_error_path("hover_exit_ms = 2001\n", "interaction.hover_exit_ms");
  }
  SECTION("unknown property") {
    check_error_path("animation_curve = \"linear\"\n", "interaction.animation_curve");
  }

  SECTION("interaction is not a table") {
    constexpr auto source = R"(monitor = "primary"
theme = "organic"
default_module = "clock"
interaction = "fast"
[[modules]]
id = "clock"
command = ["clock"]
)";
    const auto result = gisland::parse_config(source, "interaction.toml");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "interaction");
  }
}

TEST_CASE("module expanded preview duration is opt-in and bounded") {
  SECTION("absent") {
    const auto result = gisland::parse_config(config_with_module_field(""), "preview.toml");
    REQUIRE(result.has_value());
    CHECK(result->modules[0].expanded_preview == std::chrono::milliseconds{0});
  }

  SECTION("explicit values") {
    const auto disabled = gisland::parse_config(
        config_with_module_field("expanded_preview_ms = 0\n"), "preview.toml");
    REQUIRE(disabled.has_value());
    CHECK(disabled->modules[0].expanded_preview == std::chrono::milliseconds{0});

    const auto enabled = gisland::parse_config(
        config_with_module_field("expanded_preview_ms = 1000\n"), "preview.toml");
    REQUIRE(enabled.has_value());
    CHECK(enabled->modules[0].expanded_preview == std::chrono::milliseconds{1000});

    const auto maximum = gisland::parse_config(
        config_with_module_field("expanded_preview_ms = 60000\n"), "preview.toml");
    REQUIRE(maximum.has_value());
    CHECK(maximum->modules[0].expanded_preview == std::chrono::milliseconds{60000});
  }
}

TEST_CASE("invalid module expanded preview duration is rejected at the TOML boundary") {
  const auto check_error = [](std::string_view field) {
    const auto result = gisland::parse_config(config_with_module_field(field), "preview.toml");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().path == "modules[0].expanded_preview_ms");
  };

  SECTION("negative") { check_error("expanded_preview_ms = -1\n"); }
  SECTION("above maximum") { check_error("expanded_preview_ms = 60001\n"); }
  SECTION("floating point") { check_error("expanded_preview_ms = 1000.0\n"); }
  SECTION("string") { check_error("expanded_preview_ms = \"1000\"\n"); }
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
  const auto *view =
      result->modules.front().view.has_value() ? &result->modules.front().view.value() : nullptr;
  REQUIRE(view != nullptr);
  CHECK(std::holds_alternative<gisland::TemplateRow>(view->compact.value));
  const auto *expanded = view->expanded.has_value() ? &view->expanded.value() : nullptr;
  REQUIRE(expanded != nullptr);
  CHECK(std::holds_alternative<gisland::TemplateColumn>(expanded->value));
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

  const auto repeat = parse_view(
      "repeat=\"items\"\nas=\"item\"\ntemplate={type=\"text\",value=\"x\",role=\"body\"}\n");
  REQUIRE_FALSE(repeat.has_value());
  CHECK(repeat.error().path == "modules[0].view.compact");
}

TEST_CASE("module view templates reject duplicate aliases in one scope") {
  constexpr auto source = R"(
monitor = "primary"
theme = "default"
default_module = "clock"
[[modules]]
id = "clock"
command = ["clock"]
[modules.view.compact]
type = "row"
children = [
  { repeat = "first", as = "item", template = { type = "text", value = { bind = "item" }, role = "body" } },
  { repeat = "second", as = "item", template = { type = "text", value = { bind = "item" }, role = "body" } }
]
)";

  const auto result = gisland::parse_config(source, "duplicate-alias.toml");

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().path == "modules[0].view.compact.children[1].as");
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
