#include "gisland/protocol.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <string>

namespace {

[[nodiscard]] nlohmann::json parse_record(const std::string &record) {
  REQUIRE(record.ends_with('\n'));
  CHECK(std::ranges::count(record, '\n') == 1);
  return nlohmann::json::parse(record.begin(), record.end() - 1);
}

} // namespace

TEST_CASE("init messages serialize as one deterministic JSONL record") {
  const gisland::InitMessage message{
      .minimum = {.major = 1, .minor = 0},
      .maximum = {.major = 1, .minor = 3},
      .instance_id = "clock.primary",
      .capabilities = {"actions", "visibility"},
      .configuration = {{"format", "24h"}, {"show_date", true}},
      .locale = "en_GB.UTF-8",
      .timezone = "Europe/London",
  };

  const auto first = gisland::serialize_core_message(gisland::CoreMessage{message});
  const auto second = gisland::serialize_core_message(gisland::CoreMessage{message});

  CHECK(first == second);
  CHECK(parse_record(first) == nlohmann::json{
                                   {"type", "init"},
                                   {"protocol",
                                    {{"minimum", {{"major", 1}, {"minor", 0}}},
                                     {"maximum", {{"major", 1}, {"minor", 3}}}}},
                                   {"instance_id", "clock.primary"},
                                   {"capabilities", {"actions", "visibility"}},
                                   {"configuration", {{"format", "24h"}, {"show_date", true}}},
                                   {"locale", "en_GB.UTF-8"},
                                   {"timezone", "Europe/London"},
                               });
}

TEST_CASE("action messages preserve an optional typed value") {
  const gisland::ActionMessage with_value{
      .action_id = "calendar.select",
      .value = nlohmann::json{{"day", 29}, {"label", "line one\nline two"}},
  };
  const gisland::ActionMessage without_value{.action_id = "calendar.today", .value = std::nullopt};

  CHECK(parse_record(gisland::serialize_core_message(gisland::CoreMessage{with_value})) ==
        nlohmann::json{
            {"type", "action"},
            {"action_id", "calendar.select"},
            {"value", {{"day", 29}, {"label", "line one\nline two"}}},
        });
  CHECK(parse_record(gisland::serialize_core_message(gisland::CoreMessage{without_value})) ==
        nlohmann::json{{"type", "action"}, {"action_id", "calendar.today"}});
}

TEST_CASE("visibility states have stable wire names") {
  const auto hidden = gisland::serialize_core_message(
      gisland::CoreMessage{gisland::VisibilityMessage{gisland::Visibility::hidden}});
  const auto compact = gisland::serialize_core_message(
      gisland::CoreMessage{gisland::VisibilityMessage{gisland::Visibility::compact_active}});
  const auto expanded = gisland::serialize_core_message(
      gisland::CoreMessage{gisland::VisibilityMessage{gisland::Visibility::expanded_active}});

  CHECK(parse_record(hidden) == nlohmann::json{{"type", "visibility"}, {"visibility", "hidden"}});
  CHECK(parse_record(compact) ==
        nlohmann::json{{"type", "visibility"}, {"visibility", "compact-active"}});
  CHECK(parse_record(expanded) ==
        nlohmann::json{{"type", "visibility"}, {"visibility", "expanded-active"}});
}

TEST_CASE("shutdown deadlines serialize as integer milliseconds") {
  using namespace std::chrono_literals;

  const gisland::ShutdownMessage message{.reason = "application-exit", .deadline = 1250ms};

  CHECK(parse_record(gisland::serialize_core_message(gisland::CoreMessage{message})) ==
        nlohmann::json{
            {"type", "shutdown"},
            {"reason", "application-exit"},
            {"deadline_ms", 1250},
        });
}
