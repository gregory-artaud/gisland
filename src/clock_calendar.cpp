#include "gisland/clock_calendar.hpp"

#include <chrono>
#include <exception>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace gisland {
namespace {

using namespace std::chrono;

struct LocalFields {
  year_month_day date;
  hours hour;
  minutes minute;
};

[[nodiscard]] LocalFields local_fields(const time_zone *timezone, sys_seconds now) {
  const zoned_time local{timezone, now};
  const auto local_time = local.get_local_time();
  const auto local_day = floor<days>(local_time);
  const auto time = hh_mm_ss{local_time - local_day};
  return {
      .date = year_month_day{sys_days{local_day.time_since_epoch()}},
      .hour = time.hours(),
      .minute = time.minutes(),
  };
}

[[nodiscard]] std::tm calendar_time(year_month_day date, hours hour = hours{0},
                                    minutes minute = minutes{0}) {
  std::tm value{};
  value.tm_year = static_cast<int>(date.year()) - 1900;
  value.tm_mon = static_cast<int>(static_cast<unsigned int>(date.month())) - 1;
  value.tm_mday = static_cast<int>(static_cast<unsigned int>(date.day()));
  value.tm_wday = static_cast<int>(weekday{sys_days{date}}.c_encoding());
  value.tm_hour = static_cast<int>(hour.count());
  value.tm_min = static_cast<int>(minute.count());
  value.tm_isdst = -1;
  return value;
}

[[nodiscard]] std::string localized(const std::locale &locale, const std::tm &value,
                                    const char *format) {
  std::ostringstream stream;
  stream.imbue(locale);
  stream << std::put_time(&value, format);
  if (!stream) {
    throw std::runtime_error{"unable to format calendar value"};
  }
  return stream.str();
}

[[nodiscard]] std::string clock_label(hours hour, minutes minute) {
  std::ostringstream stream;
  stream << std::setfill('0') << std::setw(2) << hour.count() << ':' << std::setw(2)
         << minute.count();
  return stream.str();
}

[[nodiscard]] std::string day_label(day value) {
  std::ostringstream stream;
  stream << std::setfill('0') << std::setw(2) << static_cast<unsigned int>(value);
  return stream.str();
}

[[nodiscard]] int week_offset(weekday day, WeekStart week_start) {
  const int sunday_based = static_cast<int>(day.c_encoding());
  return week_start == WeekStart::monday ? (sunday_based + 6) % 7 : sunday_based;
}

[[nodiscard]] bool supported(year_month month) {
  const int year_value = static_cast<int>(month.year());
  return month.ok() && year_value >= 1 && year_value <= 9999;
}

} // namespace

ClockCalendarModel::ClockCalendarModel(ClockCalendarOptions options, const std::locale &locale,
                                       const time_zone *timezone, year_month displayed)
    : options_(std::move(options)), locale_(locale), timezone_(timezone),
      displayed_month_(displayed) {}

std::expected<ClockCalendarModel, std::string>
ClockCalendarModel::create(ClockCalendarOptions options, sys_seconds now) {
  try {
    if (options.locale.empty()) {
      return std::unexpected("locale must not be empty");
    }
    if (options.timezone.empty()) {
      return std::unexpected("timezone must not be empty");
    }
    std::locale locale{options.locale.c_str()};
    const time_zone *timezone = locate_zone(options.timezone);
    const auto current = local_fields(timezone, now).date;
    return ClockCalendarModel{std::move(options), locale, timezone,
                              current.year() / current.month()};
  } catch (const std::exception &error) {
    return std::unexpected(error.what());
  }
}

std::expected<ClockCalendarSnapshot, std::string> ClockCalendarModel::snapshot(sys_seconds now) {
  try {
    const auto current = local_fields(timezone_, now);
    if (follows_current_month_) {
      displayed_month_ = current.date.year() / current.date.month();
    }

    ClockCalendarSnapshot result;
    result.time = clock_label(current.hour, current.minute);
    const std::tm current_time = calendar_time(current.date, current.hour, current.minute);
    result.date_short = localized(locale_, current_time, "%a") + " " +
                        std::to_string(static_cast<unsigned int>(current.date.day())) + " " +
                        localized(locale_, current_time, "%b");

    const year_month_day first{displayed_month_ / day{1}};
    const std::tm first_time = calendar_time(first);
    result.month_label = localized(locale_, first_time, "%B") + " " +
                         std::to_string(static_cast<int>(displayed_month_.year()));

    const sys_days label_start =
        sys_days{2024y / January / 1} + days{options_.week_start == WeekStart::monday ? 0 : -1};
    for (std::size_t index = 0; index < result.weekdays.size(); ++index) {
      const year_month_day label_date{label_start + days{static_cast<int>(index)}};
      result.weekdays[index] = localized(locale_, calendar_time(label_date), "%a");
    }

    const sys_days grid_start =
        sys_days{first} - days{week_offset(weekday{sys_days{first}}, options_.week_start)};
    for (std::size_t week = 0; week < result.weeks.size(); ++week) {
      for (std::size_t day_index = 0; day_index < result.weeks[week].size(); ++day_index) {
        const auto offset = static_cast<int>((week * result.weeks[week].size()) + day_index);
        const year_month_day date{grid_start + days{offset}};
        const bool in_month = date.year() / date.month() == displayed_month_;
        const bool today = in_month && date == current.date;
        std::string role = "muted";
        if (today) {
          role = "accent";
        } else if (in_month) {
          role = "body";
        }
        result.weeks[week][day_index] = {
            .label = day_label(date.day()),
            .role = std::move(role),
        };
      }
    }
    return result;
  } catch (const std::exception &error) {
    return std::unexpected(error.what());
  }
}

std::expected<bool, std::string> ClockCalendarModel::apply_action(std::string_view action_id,
                                                                  sys_seconds now) {
  try {
    if (action_id == "today") {
      const auto current = local_fields(timezone_, now).date;
      displayed_month_ = current.year() / current.month();
      follows_current_month_ = true;
      return true;
    }
    if (action_id != "previous-month" && action_id != "next-month") {
      return false;
    }
    if (follows_current_month_) {
      const auto current = local_fields(timezone_, now).date;
      displayed_month_ = current.year() / current.month();
    }
    const year_month candidate =
        displayed_month_ + (action_id == "previous-month" ? months{-1} : months{1});
    if (!supported(candidate)) {
      return false;
    }
    displayed_month_ = candidate;
    follows_current_month_ = false;
    return true;
  } catch (const std::exception &error) {
    return std::unexpected(error.what());
  }
}

milliseconds until_next_minute(sys_time<milliseconds> now) {
  return duration_cast<milliseconds>((floor<minutes>(now) + minutes{1}) - now);
}

} // namespace gisland
