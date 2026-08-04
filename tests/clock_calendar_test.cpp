#include "gisland/clock_calendar.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <utility>

namespace {

using namespace std::chrono_literals;
using namespace std::chrono;

constexpr sys_seconds august_now = sys_days{2026y / August / 3} + 14h + 35min + 42s;

gisland::ClockCalendarModel model(gisland::WeekStart week_start = gisland::WeekStart::monday,
                                  std::string timezone = "UTC") {
  auto result = gisland::ClockCalendarModel::create(
      {.locale = "C", .timezone = std::move(timezone), .week_start = week_start}, august_now);
  REQUIRE(result.has_value());
  return std::move(*result);
}

} // namespace

TEST_CASE("clock-calendar snapshot formats live time and a stable Monday-first month") {
  auto calendar = model();
  const auto snapshot = calendar.snapshot(august_now);

  REQUIRE(snapshot.has_value());
  CHECK(snapshot->time == "14:35");
  CHECK(snapshot->date_short == "Mon 3 Aug");
  CHECK(snapshot->month_label == "August 2026");
  CHECK(snapshot->weekdays.front() == "Mon");
  CHECK(snapshot->weekdays.back() == "Sun");
  CHECK(snapshot->weeks.size() == 6);
  CHECK(snapshot->weeks.front().size() == 7);
  CHECK(snapshot->weeks.front().front().label == "27");
  CHECK(snapshot->weeks.front().front().role == "muted");
  CHECK(snapshot->weeks.front().at(5).label == "01");
  CHECK(snapshot->weeks.front().at(5).role == "body");
  CHECK(snapshot->weeks.at(1).front().label == "03");
  CHECK(snapshot->weeks.at(1).front().role == "accent");
  CHECK(snapshot->weeks.back().back().label == "06");
  CHECK(snapshot->weeks.back().back().role == "muted");
}

TEST_CASE("clock-calendar supports Sunday-first grids and timezone conversion") {
  auto calendar = model(gisland::WeekStart::sunday, "Etc/GMT-2");
  const auto late_utc = sys_days{2026y / August / 3} + 22h + 35min;
  const auto snapshot = calendar.snapshot(late_utc);

  REQUIRE(snapshot.has_value());
  CHECK(snapshot->time == "00:35");
  CHECK(snapshot->date_short == "Tue 4 Aug");
  CHECK(snapshot->weekdays.front() == "Sun");
  CHECK(snapshot->weeks.front().front().label == "26");
}

TEST_CASE("clock-calendar localizes French calendar labels") {
  auto created = gisland::ClockCalendarModel::create(
      {.locale = "fr_FR.UTF-8", .timezone = "UTC", .week_start = gisland::WeekStart::monday},
      august_now);
  REQUIRE(created.has_value());
  const auto snapshot = created->snapshot(august_now);

  REQUIRE(snapshot.has_value());
  CHECK(snapshot->date_short == "lun. 3 août");
  CHECK(snapshot->month_label == "août 2026");
  CHECK(snapshot->weekdays.front() == "lun.");
}

TEST_CASE("clock-calendar navigation handles leap years and persists across midnight") {
  auto calendar = model();

  REQUIRE(calendar.apply_action("previous-month", august_now).value_or(false));
  auto snapshot = calendar.snapshot(sys_days{2026y / September / 1} + 1min);
  REQUIRE(snapshot.has_value());
  CHECK(snapshot->month_label == "July 2026");

  REQUIRE(calendar.apply_action("today", sys_days{2024y / February / 29}).value_or(false));
  snapshot = calendar.snapshot(sys_days{2024y / February / 29});
  REQUIRE(snapshot.has_value());
  CHECK(snapshot->month_label == "February 2024");
  CHECK(snapshot->weeks.at(4).at(3).label == "29");
  CHECK(snapshot->weeks.at(4).at(3).role == "accent");

  REQUIRE(calendar.apply_action("today", sys_days{2026y / January / 15}).value_or(false));
  REQUIRE(calendar.apply_action("previous-month", sys_days{2026y / January / 15}).value_or(false));
  snapshot = calendar.snapshot(sys_days{2026y / January / 15});
  REQUIRE(snapshot.has_value());
  CHECK(snapshot->month_label == "December 2025");
  REQUIRE(calendar.apply_action("next-month", sys_days{2026y / January / 15}).value_or(false));
  snapshot = calendar.snapshot(sys_days{2026y / January / 15});
  REQUIRE(snapshot.has_value());
  CHECK(snapshot->month_label == "January 2026");

  REQUIRE_FALSE(calendar.apply_action("unknown", august_now).value_or(true));
}

TEST_CASE("clock-calendar follows the current month until navigation changes it") {
  const auto end_of_august = sys_days{2026y / August / 31} + 23h + 59min;
  auto created = gisland::ClockCalendarModel::create(
      {.locale = "C", .timezone = "UTC", .week_start = gisland::WeekStart::monday}, end_of_august);
  REQUIRE(created.has_value());
  auto calendar = std::move(*created);

  auto snapshot = calendar.snapshot(sys_days{2026y / September / 1});
  REQUIRE(snapshot.has_value());
  CHECK(snapshot->month_label == "September 2026");

  REQUIRE(calendar.apply_action("previous-month", sys_days{2026y / September / 1}).value_or(false));
  snapshot = calendar.snapshot(sys_days{2026y / October / 1});
  REQUIRE(snapshot.has_value());
  CHECK(snapshot->month_label == "August 2026");
}

TEST_CASE("clock-calendar validates locale and timezone and schedules the next minute") {
  CHECK_FALSE(
      gisland::ClockCalendarModel::create({.locale = "not-a-locale", .timezone = "UTC"}, august_now)
          .has_value());
  CHECK_FALSE(
      gisland::ClockCalendarModel::create({.locale = "C", .timezone = "not/a-zone"}, august_now)
          .has_value());

  const auto now = sys_days{2026y / August / 3} + 14h + 35min + 42s + 250ms;
  CHECK(gisland::until_next_minute(now) == 17750ms);
  CHECK(gisland::until_next_minute(sys_days{2026y / August / 3} + 14h + 36min) == 60s);
}
