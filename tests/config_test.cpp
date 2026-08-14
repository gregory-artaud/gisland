#include "gisland/config.hpp"
#include "gisland/module_manifest.hpp"

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <variant>

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    static std::uint64_t sequence{};
    path_ = std::filesystem::temp_directory_path() /
            ("gisland-config-" + std::to_string(::getpid()) + "-" + std::to_string(sequence++));
    std::filesystem::create_directories(path_);
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path &path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output{path};
  REQUIRE(output.good());
  output << content;
}

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
        config_with_interaction(
            "animation_speed = 2.0\nhover_exit_ms = 450\nreduced_motion = true\n"),
        "interaction.toml");

    REQUIRE(result.has_value());
    CHECK(result->interaction.animation_speed == 2.0);
    CHECK(result->interaction.hover_exit == std::chrono::milliseconds{450});
    CHECK(result->interaction.reduced_motion);
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
  SECTION("reduced motion type") {
    check_error_path("reduced_motion = \"yes\"\n", "interaction.reduced_motion");
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

TEST_CASE("independent defaults parse enabled compact and expanded instances") {
  constexpr auto source = R"(
monitor = "primary"
theme = "organic"
[defaults]
compact = "clock"
expanded = "calendar"
[[modules]]
id = "clock"
command = ["clock"]
[[modules]]
id = "calendar"
command = ["calendar"]
)";

  const auto result = gisland::parse_config(source, "defaults.toml");

  REQUIRE(result.has_value());
  CHECK(result->compact_default == "clock");
  CHECK(result->expanded_default == "calendar");
}

TEST_CASE("independent defaults reject legacy mixing and incomplete slot values") {
  const auto mixed = gisland::parse_config(R"(
monitor = "primary"
theme = "organic"
default_module = "clock"
[defaults]
compact = "clock"
expanded = "clock"
[[modules]]
id = "clock"
command = ["clock"]
)",
                                           "mixed.toml");
  REQUIRE_FALSE(mixed.has_value());
  CHECK(mixed.error().path == "defaults");

  const auto incomplete = gisland::parse_config(R"(
monitor = "primary"
theme = "organic"
[defaults]
compact = "clock"
[[modules]]
id = "clock"
command = ["clock"]
)",
                                                "incomplete.toml");
  REQUIRE_FALSE(incomplete.has_value());
  CHECK(incomplete.error().path == "defaults.expanded");
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

TEST_CASE("manifest-backed numeric option overrides enforce inclusive bounds") {
  const auto manifest = gisland::parse_module_manifest(R"(
id = "bounded"
name = "Bounded"
command = ["bounded"]
[protocol]
major = 1
minimum_minor = 0
maximum_minor = 8
[options_schema.count]
type = "integer"
minimum = 1
maximum = 3
[options_schema.ratio]
type = "number"
minimum = 0.5
maximum = 1.5
)",
                                                       "module.toml", "bounded");
  REQUIRE(manifest.has_value());
  gisland::ModuleCatalog catalog;
  catalog.manifests.emplace("bounded", *manifest);
  const auto parse_options = [&](std::string_view options) {
    return gisland::parse_config(std::string{R"(monitor="primary"
theme="default"
default_module="instance"
[[modules]]
id="instance"
module="bounded"
[modules.options]
)"} + std::string{options},
                                 "config.toml", catalog);
  };

  SECTION("minimum boundaries") {
    const auto parsed = parse_options("count=1\nratio=0.5\n");
    REQUIRE(parsed.has_value());
    CHECK(std::get<std::int64_t>(parsed->modules.front().options.at("count").value) == 1);
    CHECK(std::get<double>(parsed->modules.front().options.at("ratio").value) == 0.5);
  }
  SECTION("maximum boundaries") {
    const auto parsed = parse_options("count=3\nratio=1.5\n");
    REQUIRE(parsed.has_value());
  }
  SECTION("number options accept integer values") {
    const auto parsed = parse_options("count=2\nratio=1\n");
    REQUIRE(parsed.has_value());
    CHECK(std::get<std::int64_t>(parsed->modules.front().options.at("ratio").value) == 1);
  }
  SECTION("integer below minimum") {
    const auto parsed = parse_options("count=0\nratio=1.0\n");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().path == "modules[0].options.count");
  }
  SECTION("number above maximum") {
    const auto parsed = parse_options("count=2\nratio=1.6\n");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().path == "modules[0].options.ratio");
  }
  SECTION("non-finite override") {
    const auto parsed = parse_options("count=2\nratio=nan\n");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().path == "modules[0].options.ratio");
  }
}

TEST_CASE("manifest package defaults merge before per-instance option overrides") {
  const auto parsed_manifest = gisland::parse_module_manifest(R"(
id="package"
name="Package"
command=["package"]
[protocol]
major=1
minimum_minor=0
maximum_minor=8
[defaults]
mode="quiet"
level=1
[options_schema.mode]
type="string"
[options_schema.level]
type="integer"
)",
                                                              "module.toml", "package");
  REQUIRE(parsed_manifest.has_value());
  gisland::ModuleCatalog catalog;
  catalog.manifests.emplace("package", *parsed_manifest);

  const auto config = gisland::parse_config(R"(
monitor="primary"
theme="default"
default_module="first"
[[modules]]
id="first"
module="package"
[modules.options]
level=2
[[modules]]
id="second"
module="package"
[modules.options]
mode="loud"
)",
                                            "config.toml", catalog);

  REQUIRE(config.has_value());
  CHECK(std::get<std::string>(config->modules[0].options.at("mode").value) == "quiet");
  CHECK(std::get<std::int64_t>(config->modules[0].options.at("level").value) == 2);
  CHECK(std::get<std::string>(config->modules[1].options.at("mode").value) == "loud");
  CHECK(std::get<std::int64_t>(config->modules[1].options.at("level").value) == 1);
}

TEST_CASE("instance view slots replace package slots independently") {
  gisland::ModuleManifest manifest{
      .id = "package",
      .name = "Package",
      .description = {},
      .command = {"package"},
      .minimum_protocol = {1, 0},
      .maximum_protocol = {1, 8},
      .defaults = {},
      .options_schema = {},
      .path = "module.toml",
      .entry_path = std::nullopt,
      .config_path = std::nullopt,
      .view_path = std::nullopt,
      .view =
          gisland::ModuleViewConfig{
              .compact = gisland::SceneTemplate{gisland::TemplateText{"package compact", "body"}},
              .expanded = gisland::SceneTemplate{gisland::TemplateText{"package expanded", "body"}},
          },
  };
  gisland::ModuleCatalog catalog;
  catalog.manifests.emplace("package", manifest);

  const auto config = gisland::parse_config(R"(
monitor="primary"
theme="default"
default_module="instance"
[[modules]]
id="instance"
module="package"
[modules.view.expanded]
type="column"
children=[{type="text",value="override child",role="body"}]
)",
                                            "config.toml", catalog);

  REQUIRE(config.has_value());
  REQUIRE(config->modules.front().view.has_value());
  const auto &view = *config->modules.front().view;
  CHECK(std::get<std::string>(std::get<gisland::TemplateText>(view.compact.value).value) ==
        "package compact");
  const auto &expanded = std::get<gisland::TemplateColumn>(view.expanded->value);
  REQUIRE(expanded.children.size() == 1);
  CHECK(std::holds_alternative<gisland::TemplateText>(
      std::get<gisland::SceneTemplatePtr>(expanded.children.front())->value));

  const auto compact_override = gisland::parse_config(R"(
monitor="primary"
theme="default"
default_module="instance"
[[modules]]
id="instance"
module="package"
[modules.view.compact]
type="text"
value="instance compact"
role="body"
)",
                                                      "config.toml", catalog);
  REQUIRE(compact_override.has_value());
  REQUIRE(compact_override->modules.front().view.has_value());
  const auto &compact =
      std::get<gisland::TemplateText>(compact_override->modules.front().view->compact.value);
  CHECK(std::get<std::string>(compact.value) == "instance compact");
  const auto &retained_expanded =
      std::get<gisland::TemplateText>(compact_override->modules.front().view->expanded->value);
  CHECK(std::get<std::string>(retained_expanded.value) == "package expanded");
}

TEST_CASE("package views are copied safely into independently resolved instances") {
  gisland::ModuleManifest manifest{
      .id = "package",
      .name = "Package",
      .description = {},
      .command = {"package"},
      .minimum_protocol = {1, 0},
      .maximum_protocol = {1, 8},
      .defaults = {},
      .options_schema = {},
      .path = "module.toml",
      .entry_path = std::nullopt,
      .config_path = std::nullopt,
      .view_path = std::nullopt,
      .view =
          gisland::ModuleViewConfig{
              .compact = gisland::SceneTemplate{gisland::TemplateText{
                  {gisland::DataBinding{"label"}}, "body"}},
              .expanded = std::nullopt,
          },
  };
  gisland::ModuleCatalog catalog;
  catalog.manifests.emplace("package", manifest);
  auto copied_catalog = catalog;

  const auto config = gisland::parse_config(R"(
monitor="primary"
theme="default"
default_module="first"
[[modules]]
id="first"
module="package"
[[modules]]
id="second"
module="package"
[modules.view.compact]
type="text"
value="second"
role="body"
)",
                                            "config.toml", copied_catalog);

  REQUIRE(config.has_value());
  REQUIRE(config->modules[0].view.has_value());
  REQUIRE(config->modules[1].view.has_value());
  const auto &first = std::get<gisland::TemplateText>(config->modules[0].view->compact.value);
  const auto &second = std::get<gisland::TemplateText>(config->modules[1].view->compact.value);
  CHECK(std::holds_alternative<gisland::DataBinding>(first.value));
  CHECK(std::get<std::string>(second.value) == "second");
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

TEST_CASE("manifest Lua entries precede command and instance arguments") {
  gisland::ModuleCatalog catalog;
  catalog.manifests.emplace("lua-package",
                            gisland::ModuleManifest{
                                .id = "lua-package",
                                .name = "Lua package",
                                .description = {},
                                .command = {"gisland-lua-host", "--manifest-option"},
                                .minimum_protocol = {1, 8},
                                .maximum_protocol = {1, 8},
                                .defaults = {},
                                .options_schema = {},
                                .path = "/packages/lua-package/module.toml",
                                .entry_path = "/packages/lua-package/entry.lua",
                                .config_path = std::nullopt,
                                .view_path = std::nullopt,
                                .view = std::nullopt,
                                .dependencies = {.manifest = std::nullopt,
                                                 .config = std::nullopt,
                                                 .view = std::nullopt,
                                                 .entry =
                                                     gisland::ModuleFileDependency{
                                                         .path = "/packages/lua-package/entry.lua",
                                                         .fingerprint = "0123456789abcdef"}},
                            });

  const auto result = gisland::parse_config(R"(
monitor = "primary"
theme = "organic"
default_module = "lua"
[[modules]]
id = "lua"
module = "lua-package"
arguments = ["--user-option", "replacement.lua"]
)",
                                            "config.toml", catalog);

  REQUIRE(result.has_value());
  REQUIRE(result->modules.size() == 1);
  CHECK(result->modules.front().command ==
        std::vector<std::string>{"gisland-lua-host", "/packages/lua-package/entry.lua",
                                 "--gisland-entry-fingerprint=0123456789abcdef",
                                 "--manifest-option", "--user-option", "replacement.lua"});
}

TEST_CASE("manifest-backed configuration rejects package replacement after discovery") {
  TemporaryDirectory temporary;
  const auto package = temporary.path() / "module";
  write_file(package / "entry.lua", "return gisland.module {}\n");
  write_file(package / "module.toml", R"(
id = "module"
name = "Module"
command = ["gisland-lua-host"]
entry = "entry.lua"
[protocol]
major = 1
minimum_minor = 8
maximum_minor = 8
)");
  auto catalog = gisland::discover_module_catalog(
      temporary.path() / "missing-config", temporary.path() / "missing-data", temporary.path());
  REQUIRE(catalog.manifests.contains("module"));
  write_file(package / "entry.lua", "return gisland.module { init = function() end }\n");

  const auto result = gisland::parse_config(R"(
monitor = "primary"
theme = "organic"
default_module = "instance"
[[modules]]
id = "instance"
module = "module"
)",
                                            "config.toml", catalog);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().path == "modules[0].module");
  CHECK(result.error().message == "module package changed during configuration");
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
