#include "gisland/module_manifest.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() / ("gisland-manifest-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }
  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path &path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream{path};
  if (!stream) {
    throw std::runtime_error{"could not create manifest fixture"};
  }
  stream << content;
}

constexpr std::string_view manifest = R"(
id = "clock-calendar"
name = "Clock and Calendar"
description = "Localized clock"
command = ["gisland-clock-calendar", "--jsonl"]

[protocol]
major = 1
minimum_minor = 0
maximum_minor = 1

[defaults]
week_start = "monday"

[options_schema.week_start]
type = "string"
allowed = ["monday", "sunday"]
)";

} // namespace

TEST_CASE("module manifest parses typed command protocol defaults and schema") {
  const auto parsed = gisland::parse_module_manifest(manifest, "module.toml", "clock-calendar");

  REQUIRE(parsed.has_value());
  CHECK(parsed->id == "clock-calendar");
  CHECK(parsed->name == "Clock and Calendar");
  CHECK((parsed->command == std::vector<std::string>{"gisland-clock-calendar", "--jsonl"}));
  CHECK(parsed->minimum_protocol == gisland::ProtocolVersion{1, 0});
  CHECK(parsed->maximum_protocol == gisland::ProtocolVersion{1, 1});
  CHECK(std::get<std::string>(parsed->defaults.at("week_start").value) == "monday");
  CHECK(parsed->options_schema.at("week_start").type == gisland::ModuleOptionType::string);
  CHECK((parsed->options_schema.at("week_start").allowed ==
         std::vector<std::string>{"monday", "sunday"}));
}

TEST_CASE("loaded module manifests resolve package file references") {
  TemporaryDirectory temporary;
  const auto package = temporary.path() / "lua-clock";
  write_file(package / "clock.lua", "return {}\n");
  write_file(package / "config.toml", "[defaults]\nmode=\"quiet\"\n");
  write_file(package / "view.toml", "[compact]\ntype=\"text\"\nvalue=\"Clock\"\nrole=\"body\"\n");
  write_file(package / "module.toml", R"(
id = "lua-clock"
name = "Lua Clock"
command = ["/usr/libexec/gisland-lua-host"]
entry = "clock.lua"
config = "config.toml"
view = "view.toml"
[protocol]
major = 1
minimum_minor = 8
maximum_minor = 8
[options_schema.mode]
type = "string"
)");

  const auto loaded = gisland::load_module_manifest(package / "module.toml", "lua-clock");

  REQUIRE(loaded.has_value());
  REQUIRE(loaded->entry_path.has_value());
  REQUIRE(loaded->config_path.has_value());
  REQUIRE(loaded->view_path.has_value());
  CHECK(*loaded->entry_path == std::filesystem::canonical(package / "clock.lua"));
  CHECK(*loaded->config_path == std::filesystem::canonical(package / "config.toml"));
  CHECK(*loaded->view_path == std::filesystem::canonical(package / "view.toml"));
}

TEST_CASE("referenced package defaults use one exact defaults table") {
  TemporaryDirectory temporary;
  const auto package = temporary.path() / "packaged";
  write_file(package / "module.toml", R"(
id = "packaged"
name = "Packaged"
command = ["packaged"]
config = "config.toml"
[protocol]
major = 1
minimum_minor = 0
maximum_minor = 8
[options_schema.mode]
type = "string"
allowed = ["quiet", "loud"]
[options_schema.level]
type = "integer"
minimum = 1
maximum = 3
)");

  const auto load_defaults = [&](std::string_view contents) {
    write_file(package / "config.toml", contents);
    return gisland::load_module_manifest(package / "module.toml", "packaged");
  };

  const auto valid = load_defaults("[defaults]\nmode=\"quiet\"\nlevel=2\n");
  REQUIRE(valid.has_value());
  CHECK(std::get<std::string>(valid->defaults.at("mode").value) == "quiet");
  CHECK(std::get<std::int64_t>(valid->defaults.at("level").value) == 2);

  SECTION("unknown top-level table") {
    const auto loaded = load_defaults("[defaults]\nmode=\"quiet\"\n[extra]\nvalue=1\n");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().source == package / "config.toml");
    CHECK(loaded.error().path == "extra");
  }
  SECTION("unknown default") {
    const auto loaded = load_defaults("[defaults]\nunknown=true\n");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "defaults.unknown");
  }
  SECTION("wrong type") {
    const auto loaded = load_defaults("[defaults]\nmode=1\n");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "defaults.mode");
    CHECK(loaded.error().line == 2);
    CHECK(loaded.error().column > 0);
  }
  SECTION("disallowed value") {
    const auto loaded = load_defaults("[defaults]\nmode=\"medium\"\n");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "defaults.mode");
  }
  SECTION("numeric bound") {
    const auto loaded = load_defaults("[defaults]\nlevel=4\n");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "defaults.level");
  }
}

TEST_CASE("referenced and inline package defaults are mutually exclusive") {
  TemporaryDirectory temporary;
  const auto package = temporary.path() / "duplicate";
  write_file(package / "config.toml", "[defaults]\nmode=\"quiet\"\n");
  write_file(package / "module.toml", R"(
id = "duplicate"
name = "Duplicate"
command = ["duplicate"]
config = "config.toml"
[protocol]
major = 1
minimum_minor = 0
maximum_minor = 8
[defaults]
mode = "loud"
[options_schema.mode]
type = "string"
)");

  const auto loaded = gisland::load_module_manifest(package / "module.toml", "duplicate");

  REQUIRE_FALSE(loaded.has_value());
  CHECK(loaded.error().source == package / "module.toml");
  CHECK(loaded.error().path == "defaults");
}

TEST_CASE("referenced package views load optional typed slots") {
  TemporaryDirectory temporary;
  const auto package = temporary.path() / "views";
  write_file(package / "view.toml", R"(
[compact]
type = "text"
value = { bind = "label" }
role = "body"
[expanded]
type = "column"
children = [{ type = "text", value = "Details", role = "heading" }]
)");
  write_file(package / "module.toml", R"(
id = "views"
name = "Views"
command = ["views"]
view = "view.toml"
[protocol]
major = 1
minimum_minor = 0
maximum_minor = 8
)");

  const auto loaded = gisland::load_module_manifest(package / "module.toml", "views");

  REQUIRE(loaded.has_value());
  REQUIRE(loaded->view.has_value());
  REQUIRE(loaded->view->compact.has_value());
  REQUIRE(loaded->view->expanded.has_value());
  CHECK(std::holds_alternative<gisland::TemplateText>(loaded->view->compact->value));
  CHECK(std::holds_alternative<gisland::TemplateColumn>(loaded->view->expanded->value));
}

TEST_CASE("referenced package views reject invalid exact grammar with source diagnostics") {
  TemporaryDirectory temporary;
  const auto package = temporary.path() / "views";
  write_file(package / "module.toml", R"(
id = "views"
name = "Views"
command = ["views"]
view = "view.toml"
[protocol]
major = 1
minimum_minor = 0
maximum_minor = 8
)");
  const auto load_view = [&](std::string_view contents) {
    write_file(package / "view.toml", contents);
    return gisland::load_module_manifest(package / "module.toml", "views");
  };

  SECTION("unknown root") {
    const auto loaded = load_view("[other]\ntype=\"text\"\nvalue=\"x\"\nrole=\"body\"\n");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().source == package / "view.toml");
    CHECK(loaded.error().path == "other");
    CHECK(loaded.error().line > 0);
  }
  SECTION("root must be a table") {
    const auto loaded = load_view("compact=\"invalid\"\n");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "compact");
  }
  SECTION("unsupported primitive") {
    const auto loaded = load_view("[compact]\ntype=\"image\"\nsource=\"x\"\n");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().source == package / "view.toml");
    CHECK(loaded.error().path == "compact.type");
    CHECK(loaded.error().line > 0);
    CHECK(loaded.error().column > 0);
  }
  SECTION("at least one slot") {
    const auto loaded = load_view("# empty\n");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path.empty());
  }
}

TEST_CASE("Lua host manifests require an entry and other commands reject one") {
  TemporaryDirectory temporary;
  const auto package = temporary.path() / "module";
  write_file(package / "module.toml", R"(
id = "module"
name = "Module"
command = ["gisland-lua-host"]
[protocol]
major = 1
minimum_minor = 8
maximum_minor = 8
)");

  const auto missing = gisland::load_module_manifest(package / "module.toml", "module");
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().path == "entry");

  write_file(package / "module.lua", "return {}\n");
  write_file(package / "module.toml", R"(
id = "module"
name = "Module"
command = ["native-module"]
entry = "module.lua"
[protocol]
major = 1
minimum_minor = 0
maximum_minor = 8
)");
  const auto unexpected = gisland::load_module_manifest(package / "module.toml", "module");
  REQUIRE_FALSE(unexpected.has_value());
  CHECK(unexpected.error().path == "entry");
}

TEST_CASE("package file references remain contained regular files") {
  TemporaryDirectory temporary;
  const auto package = temporary.path() / "module";
  const auto outside = temporary.path() / "outside.lua";
  write_file(outside, "return {}\n");
  std::filesystem::create_directories(package);
  std::filesystem::create_symlink(outside, package / "escape.lua");

  const auto load_entry = [&](std::string_view entry) {
    write_file(package / "module.toml", std::string{R"(id = "module"
name = "Module"
command = ["gisland-lua-host"]
entry = ")"} + std::string{entry} + R"("
[protocol]
major = 1
minimum_minor = 8
maximum_minor = 8
)");
    return gisland::load_module_manifest(package / "module.toml", "module");
  };

  SECTION("absolute path") {
    const auto loaded = load_entry(outside.string());
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "entry");
  }
  SECTION("parent component") {
    const auto loaded = load_entry("../outside.lua");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "entry");
  }
  SECTION("missing file") {
    const auto loaded = load_entry("missing.lua");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "entry");
  }
  SECTION("directory") {
    std::filesystem::create_directories(package / "directory.lua");
    const auto loaded = load_entry("directory.lua");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "entry");
  }
  SECTION("symlink escape") {
    const auto loaded = load_entry("escape.lua");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "entry");
  }
  SECTION("missing config") {
    write_file(package / "module.lua", "return {}\n");
    write_file(package / "module.toml", R"(
id = "module"
name = "Module"
command = ["gisland-lua-host"]
entry = "module.lua"
config = "missing.toml"
[protocol]
major = 1
minimum_minor = 8
maximum_minor = 8
)");
    const auto loaded = gisland::load_module_manifest(package / "module.toml", "module");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "config");
  }
  SECTION("view symlink escape") {
    write_file(package / "module.lua", "return {}\n");
    std::filesystem::create_symlink(outside, package / "escape-view.toml");
    write_file(package / "module.toml", R"(
id = "module"
name = "Module"
command = ["gisland-lua-host"]
entry = "module.lua"
view = "escape-view.toml"
[protocol]
major = 1
minimum_minor = 8
maximum_minor = 8
)");
    const auto loaded = gisland::load_module_manifest(package / "module.toml", "module");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "view");
  }
  SECTION("package path contains a Lua path separator") {
    const auto unsafe_package = temporary.path() / "unsafe;parent/module";
    write_file(unsafe_package / "module.lua", "return {}\n");
    write_file(unsafe_package / "module.toml", R"(
id = "module"
name = "Module"
command = ["gisland-lua-host"]
entry = "module.lua"
[protocol]
major = 1
minimum_minor = 8
maximum_minor = 8
)");
    const auto loaded = gisland::load_module_manifest(unsafe_package / "module.toml", "module");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "entry");
  }
  SECTION("package path contains a Lua substitution marker") {
    const auto unsafe_package = temporary.path() / "unsafe?parent/module";
    write_file(unsafe_package / "module.lua", "return {}\n");
    write_file(unsafe_package / "module.toml", R"(
id = "module"
name = "Module"
command = ["gisland-lua-host"]
entry = "module.lua"
[protocol]
major = 1
minimum_minor = 8
maximum_minor = 8
)");
    const auto loaded = gisland::load_module_manifest(unsafe_package / "module.toml", "module");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "entry");
  }
}

TEST_CASE("synthetic manifest parsing does not fabricate resolved package paths") {
  const auto parsed = gisland::parse_module_manifest(R"(
id = "module"
name = "Module"
command = ["gisland-lua-host"]
entry = "module.lua"
[protocol]
major = 1
minimum_minor = 8
maximum_minor = 8
)",
                                                     "synthetic/module.toml", "module");

  REQUIRE_FALSE(parsed.has_value());
  CHECK(parsed.error().path == "entry");
}

TEST_CASE("module manifest rejects directory identity and invalid defaults") {
  const auto wrong_id = gisland::parse_module_manifest(manifest, "module.toml", "weather");
  REQUIRE_FALSE(wrong_id.has_value());
  CHECK(wrong_id.error().path == "id");

  std::string invalid{manifest};
  const auto value = invalid.find("week_start = \"monday\"");
  REQUIRE(value != std::string::npos);
  invalid.replace(value, std::string_view{"week_start = \"monday\""}.size(), "week_start = 1");
  const auto wrong_default =
      gisland::parse_module_manifest(invalid, "module.toml", "clock-calendar");
  REQUIRE_FALSE(wrong_default.has_value());
  CHECK(wrong_default.error().path == "defaults.week_start");
}

TEST_CASE("numeric option schemas parse typed inclusive bounds") {
  constexpr auto bounded = R"(
id = "bounded"
name = "Bounded"
command = ["bounded"]
[protocol]
major = 1
minimum_minor = 0
maximum_minor = 8
[defaults]
count = 1
ratio = 1.5
[options_schema.count]
type = "integer"
minimum = 1
maximum = 3
[options_schema.ratio]
type = "number"
minimum = 0.5
maximum = 1.5
)";

  const auto parsed = gisland::parse_module_manifest(bounded, "module.toml", "bounded");

  REQUIRE(parsed.has_value());
  const auto &count = parsed->options_schema.at("count");
  REQUIRE(count.minimum.has_value());
  REQUIRE(count.maximum.has_value());
  CHECK(std::get<std::int64_t>(*count.minimum) == 1);
  CHECK(std::get<std::int64_t>(*count.maximum) == 3);
  const auto &ratio = parsed->options_schema.at("ratio");
  REQUIRE(ratio.minimum.has_value());
  REQUIRE(ratio.maximum.has_value());
  CHECK(std::get<double>(*ratio.minimum) == 0.5);
  CHECK(std::get<double>(*ratio.maximum) == 1.5);
}

TEST_CASE("numeric option schema bounds reject invalid declarations and defaults") {
  const auto parse_schema = [](std::string_view type, std::string_view bounds,
                               std::string_view default_value = "1") {
    return gisland::parse_module_manifest(std::string{R"(id="bounded"
name="Bounded"
command=["bounded"]
[protocol]
major=1
minimum_minor=0
maximum_minor=8
[defaults]
value=)"} + std::string{default_value} + R"(
[options_schema.value]
type=")" + std::string{type} + "\"\n" + std::string{bounds},
                                          "module.toml", "bounded");
  };

  SECTION("bounds require a numeric option") {
    const auto parsed = parse_schema("string", "minimum=1\n", "\"x\"");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().path == "options_schema.value.minimum");
  }
  SECTION("integer bounds require integers") {
    const auto parsed = parse_schema("integer", "minimum=0.5\n");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().path == "options_schema.value.minimum");
  }
  SECTION("bounds must be finite") {
    const auto parsed = parse_schema("number", "maximum=inf\n");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().path == "options_schema.value.maximum");
  }
  SECTION("number bounds require numbers") {
    const auto parsed = parse_schema("number", "minimum=\"low\"\n");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().path == "options_schema.value.minimum");
  }
  SECTION("minimum must not exceed maximum") {
    const auto parsed = parse_schema("number", "minimum=2\nmaximum=1.5\n");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().path == "options_schema.value.minimum");
  }
  SECTION("defaults obey bounds") {
    const auto parsed = parse_schema("integer", "minimum=1\nmaximum=3\n", "4");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().path == "defaults.value");
  }
  SECTION("number defaults must be finite") {
    const auto parsed = parse_schema("number", "", "nan");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().path == "defaults.value");
  }
}

TEST_CASE("module manifest bounds metadata commands and protocol fields") {
  std::string oversized_name{manifest};
  const auto name = oversized_name.find("Clock and Calendar");
  REQUIRE(name != std::string::npos);
  oversized_name.replace(name, std::string_view{"Clock and Calendar"}.size(),
                         std::string(129, 'n'));
  const auto rejected_name =
      gisland::parse_module_manifest(oversized_name, "module.toml", "clock-calendar");
  REQUIRE_FALSE(rejected_name.has_value());
  CHECK(rejected_name.error().path == "name");

  std::string oversized_command{manifest};
  const auto executable = oversized_command.find("gisland-clock-calendar");
  REQUIRE(executable != std::string::npos);
  oversized_command.replace(executable, std::string_view{"gisland-clock-calendar"}.size(),
                            std::string(4097, 'x'));
  const auto rejected_command =
      gisland::parse_module_manifest(oversized_command, "module.toml", "clock-calendar");
  REQUIRE_FALSE(rejected_command.has_value());
  CHECK(rejected_command.error().path == "command[0]");

  std::string unknown_protocol{manifest};
  const auto protocol_end = unknown_protocol.find("\n\n[defaults]");
  REQUIRE(protocol_end != std::string::npos);
  unknown_protocol.insert(protocol_end, "\nunknown = 1");
  const auto rejected_property =
      gisland::parse_module_manifest(unknown_protocol, "module.toml", "clock-calendar");
  REQUIRE_FALSE(rejected_property.has_value());
  CHECK(rejected_property.error().path == "protocol.unknown");

  std::string overflowing_protocol{manifest};
  const auto major = overflowing_protocol.find("major = 1");
  REQUIRE(major != std::string::npos);
  overflowing_protocol.replace(
      major, std::string_view{"major = 1"}.size(),
      "major = " + std::to_string(static_cast<long long>(std::numeric_limits<int>::max()) + 1));
  const auto rejected_protocol =
      gisland::parse_module_manifest(overflowing_protocol, "module.toml", "clock-calendar");
  REQUIRE_FALSE(rejected_protocol.has_value());
  CHECK(rejected_protocol.error().path == "protocol");
}

TEST_CASE("module catalog gives invalid user overrides priority over distributed manifests") {
  TemporaryDirectory temporary;
  const auto user = temporary.path() / "user";
  const auto distributed = temporary.path() / "distributed";
  write_file(distributed / "clock-calendar/module.toml", manifest);
  write_file(user / "clock-calendar/module.toml", "id = [\n");
  write_file(distributed / "weather/module.toml",
             "id=\"weather\"\nname=\"Weather\"\ncommand=[\"weather\"]\n"
             "[protocol]\nmajor=1\nminimum_minor=0\nmaximum_minor=1\n");

  const auto catalog =
      gisland::discover_module_catalog(temporary.path() / "config", user, distributed);

  CHECK_FALSE(catalog.manifests.contains("clock-calendar"));
  CHECK(catalog.errors.contains("clock-calendar"));
  CHECK(catalog.manifests.contains("weather"));
}

TEST_CASE("module catalog gives config modules priority over data and distributed modules") {
  TemporaryDirectory temporary;
  const auto config = temporary.path() / "config";
  const auto data = temporary.path() / "data";
  const auto distributed = temporary.path() / "distributed";
  write_file(config / "clock-calendar/module.toml", manifest);

  std::string data_manifest{manifest};
  const auto data_name = data_manifest.find("Clock and Calendar");
  REQUIRE(data_name != std::string::npos);
  data_manifest.replace(data_name, std::string_view{"Clock and Calendar"}.size(), "Data Clock");
  write_file(data / "clock-calendar/module.toml", data_manifest);

  std::string distributed_manifest{manifest};
  const auto distributed_name = distributed_manifest.find("Clock and Calendar");
  REQUIRE(distributed_name != std::string::npos);
  distributed_manifest.replace(distributed_name, std::string_view{"Clock and Calendar"}.size(),
                               "Distributed Clock");
  write_file(distributed / "clock-calendar/module.toml", distributed_manifest);

  const auto catalog = gisland::discover_module_catalog(config, data, distributed);

  REQUIRE(catalog.manifests.contains("clock-calendar"));
  CHECK(catalog.manifests.at("clock-calendar").name == "Clock and Calendar");
  CHECK(catalog.manifests.at("clock-calendar").path == config / "clock-calendar/module.toml");
}

TEST_CASE("invalid config module blocks lower precedence manifests with the same ID") {
  TemporaryDirectory temporary;
  const auto config = temporary.path() / "config";
  const auto data = temporary.path() / "data";
  const auto distributed = temporary.path() / "distributed";
  write_file(config / "clock-calendar/module.toml", "id = [\n");
  write_file(data / "clock-calendar/module.toml", manifest);
  write_file(distributed / "clock-calendar/module.toml", manifest);

  const auto catalog = gisland::discover_module_catalog(config, data, distributed);

  CHECK_FALSE(catalog.manifests.contains("clock-calendar"));
  REQUIRE(catalog.errors.contains("clock-calendar"));
  CHECK(catalog.errors.at("clock-calendar").source == config / "clock-calendar/module.toml");
}

TEST_CASE("manifest-backed config resolves defaults arguments and protocol intersection") {
  auto parsed = gisland::parse_module_manifest(manifest, "module.toml", "clock-calendar");
  REQUIRE(parsed.has_value());
  gisland::ModuleCatalog catalog;
  catalog.manifests.emplace(parsed->id, std::move(*parsed));
  constexpr auto config = R"(
monitor = "primary"
theme = "default"
default_module = "clock"
[[modules]]
id = "clock"
module = "clock-calendar"
arguments = ["--verbose"]
[modules.options]
week_start = "sunday"
)";

  const auto resolved = gisland::parse_config(config, "config.toml", catalog);

  REQUIRE(resolved.has_value());
  REQUIRE(resolved->modules.size() == 1);
  const auto &instance = resolved->modules.front();
  CHECK(instance.module_id == "clock-calendar");
  CHECK((instance.command ==
         std::vector<std::string>{"gisland-clock-calendar", "--jsonl", "--verbose"}));
  CHECK(instance.minimum_protocol == gisland::ProtocolVersion{1, 0});
  CHECK(instance.maximum_protocol == gisland::ProtocolVersion{1, 1});
  CHECK(std::get<std::string>(instance.options.at("week_start").value) == "sunday");

  auto protocol_1_6 = catalog;
  protocol_1_6.manifests.at("clock-calendar").minimum_protocol = {1, 6};
  protocol_1_6.manifests.at("clock-calendar").maximum_protocol = {1, 6};
  const auto resolved_1_6 = gisland::parse_config(config, "config.toml", protocol_1_6);
  REQUIRE(resolved_1_6.has_value());
  CHECK(resolved_1_6->modules.front().minimum_protocol == gisland::ProtocolVersion{1, 6});
  CHECK(resolved_1_6->modules.front().maximum_protocol == gisland::ProtocolVersion{1, 6});

  auto protocol_1_8 = catalog;
  protocol_1_8.manifests.at("clock-calendar").minimum_protocol = {1, 8};
  protocol_1_8.manifests.at("clock-calendar").maximum_protocol = {1, 8};
  const auto resolved_1_8 = gisland::parse_config(config, "config.toml", protocol_1_8);
  REQUIRE(resolved_1_8.has_value());
  CHECK(resolved_1_8->modules.front().minimum_protocol == gisland::ProtocolVersion{1, 8});
  CHECK(resolved_1_8->modules.front().maximum_protocol == gisland::ProtocolVersion{1, 8});

  auto protocol_1_9 = catalog;
  protocol_1_9.manifests.at("clock-calendar").minimum_protocol = {1, 9};
  protocol_1_9.manifests.at("clock-calendar").maximum_protocol = {1, 9};
  const auto resolved_1_9 = gisland::parse_config(config, "config.toml", protocol_1_9);
  REQUIRE(resolved_1_9.has_value());
  CHECK(resolved_1_9->modules.front().minimum_protocol == gisland::ProtocolVersion{1, 9});
  CHECK(resolved_1_9->modules.front().maximum_protocol == gisland::ProtocolVersion{1, 9});
}

TEST_CASE("manifest-backed config rejects unknown options and incompatible protocols") {
  auto parsed = gisland::parse_module_manifest(manifest, "module.toml", "clock-calendar");
  REQUIRE(parsed.has_value());
  gisland::ModuleCatalog catalog;
  catalog.manifests.emplace(parsed->id, std::move(*parsed));
  constexpr auto unknown = R"(
monitor="primary"
theme="default"
default_module="clock"
[[modules]]
id="clock"
module="clock-calendar"
[modules.options]
colour="red"
)";
  const auto rejected = gisland::parse_config(unknown, "config.toml", catalog);
  REQUIRE_FALSE(rejected.has_value());
  CHECK(rejected.error().path == "modules[0].options.colour");

  auto incompatible = catalog;
  incompatible.manifests.at("clock-calendar").minimum_protocol = {2, 0};
  incompatible.manifests.at("clock-calendar").maximum_protocol = {2, 0};
  const auto protocol =
      gisland::parse_config("monitor=\"primary\"\ntheme=\"default\"\ndefault_module=\"clock\"\n"
                            "[[modules]]\nid=\"clock\"\nmodule=\"clock-calendar\"\n",
                            "config.toml", incompatible);
  REQUIRE_FALSE(protocol.has_value());
  CHECK(protocol.error().path == "modules[0].module");
}
