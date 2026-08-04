#pragma once

#include <array>
#include <chrono>
#include <expected>
#include <locale>
#include <string>
#include <string_view>

namespace gisland {

enum class WeekStart { monday, sunday };

struct ClockCalendarOptions {
  std::string locale;
  std::string timezone;
  WeekStart week_start{WeekStart::monday};
};

struct CalendarDay {
  std::string label;
  std::string role;
};

using CalendarWeek = std::array<CalendarDay, 7>;

struct ClockCalendarSnapshot {
  std::string time;
  std::string date_short;
  std::string month_label;
  std::array<std::string, 7> weekdays;
  std::array<CalendarWeek, 6> weeks;
};

class ClockCalendarModel {
public:
  [[nodiscard]] static std::expected<ClockCalendarModel, std::string>
  create(ClockCalendarOptions options, std::chrono::sys_seconds now);

  [[nodiscard]] std::expected<ClockCalendarSnapshot, std::string>
  snapshot(std::chrono::sys_seconds now);
  [[nodiscard]] std::expected<bool, std::string> apply_action(std::string_view action_id,
                                                              std::chrono::sys_seconds now);

private:
  ClockCalendarModel(ClockCalendarOptions options, const std::locale &locale,
                     const std::chrono::time_zone *timezone, std::chrono::year_month displayed);

  ClockCalendarOptions options_;
  std::locale locale_;
  const std::chrono::time_zone *timezone_;
  std::chrono::year_month displayed_month_;
  bool follows_current_month_{true};
};

[[nodiscard]] std::chrono::milliseconds
until_next_minute(std::chrono::sys_time<std::chrono::milliseconds> now);

} // namespace gisland
