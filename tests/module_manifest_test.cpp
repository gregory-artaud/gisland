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
