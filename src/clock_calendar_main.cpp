#include "gisland/clock_calendar.hpp"

#include <nlohmann/json.hpp>

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace {

using Json = nlohmann::json;
using namespace std::chrono;

constexpr std::size_t maximum_line_size = std::size_t{1024} * 1024;

enum class PublishResult : std::uint8_t { published, skipped, output_failed };

struct ParsedInit {
  gisland::ClockCalendarOptions options;
  int protocol_minor;
  bool content_transitions;
};

[[nodiscard]] Json snapshot_json(const gisland::ClockCalendarSnapshot &snapshot) {
  Json weeks = Json::array();
  for (const auto &week : snapshot.weeks) {
    Json days = Json::array();
    for (const auto &day : week) {
      days.push_back({{"label", day.label}, {"role", day.role}});
    }
    weeks.push_back(std::move(days));
  }
  Json calendar_columns = Json::array();
  for (std::size_t day_index = 0; day_index < snapshot.weekdays.size(); ++day_index) {
    Json days = Json::array();
    for (const auto &week : snapshot.weeks) {
      days.push_back({{"label", week[day_index].label}, {"role", week[day_index].role}});
    }
    calendar_columns.push_back(
        {{"weekday", snapshot.weekdays[day_index]}, {"days", std::move(days)}});
  }
  return {
      {"time", snapshot.time},
      {"date_short", snapshot.date_short},
      {"month_label", snapshot.month_label},
      {"weekdays", snapshot.weekdays},
      {"weeks", std::move(weeks)},
      {"calendar_columns", std::move(calendar_columns)},
  };
}

[[nodiscard]] bool write_json(const Json &message) {
  std::cout << message.dump() << '\n' << std::flush;
  return std::cout.good();
}

[[nodiscard]] std::expected<ParsedInit, std::string>
parse_init(const Json &message) {
  try {
    if (!message.is_object() || message.value("type", "") != "init") {
      return std::unexpected("first message must be init");
    }
    const auto &protocol = message.at("protocol");
    const auto &minimum = protocol.at("minimum");
    const auto &maximum = protocol.at("maximum");
    const int minimum_major = minimum.at("major").get<int>();
    const int minimum_minor = minimum.at("minor").get<int>();
    const int maximum_major = maximum.at("major").get<int>();
    const int maximum_minor = maximum.at("minor").get<int>();
    const bool supported_range =
        minimum_major == 1 && minimum_minor <= 9 && maximum_major == 1 && maximum_minor >= 1;
    if (!supported_range) {
      return std::unexpected("protocol range does not overlap 1.1 through 1.9");
    }
    const auto &capabilities = message.at("capabilities");
    if (!capabilities.is_array() || std::ranges::none_of(capabilities, [](const Json &capability) {
          return capability.is_string() &&
                 capability.get_ref<const std::string &>() == "data-snapshots";
        })) {
      return std::unexpected("data-snapshots capability is required");
    }
    const bool content_transitions =
        minimum_minor <= 9 && maximum_minor >= 9 &&
        std::ranges::any_of(capabilities, [](const Json &capability) {
          return capability.is_string() &&
                 capability.get_ref<const std::string &>() == "content-transitions";
        });

    gisland::ClockCalendarOptions options{
        .locale = message.at("locale").get<std::string>(),
        .timezone = message.at("timezone").get<std::string>(),
    };
    const auto &configuration = message.at("configuration");
    if (!configuration.is_object()) {
      return std::unexpected("configuration must be an object");
    }
    for (const auto &[key, value] : configuration.items()) {
      if (key == "locale") {
        options.locale = value.get<std::string>();
      } else if (key == "timezone") {
        options.timezone = value.get<std::string>();
      } else if (key == "week_start") {
        const auto week_start = value.get<std::string>();
        if (week_start == "monday") {
          options.week_start = gisland::WeekStart::monday;
        } else if (week_start == "sunday") {
          options.week_start = gisland::WeekStart::sunday;
        } else {
          return std::unexpected("week_start must be monday or sunday");
        }
      } else {
        return std::unexpected("unknown configuration property: " + key);
      }
    }
    return ParsedInit{std::move(options), std::min(maximum_minor, 9), content_transitions};
  } catch (const std::exception &error) {
    return std::unexpected(error.what());
  }
}

[[nodiscard]] PublishResult publish(gisland::ClockCalendarModel &model, sys_seconds now,
                                    std::optional<std::string_view> expanded_transition = {}) {
  const auto snapshot = model.snapshot(now);
  if (!snapshot) {
    std::cerr << "clock-calendar: " << snapshot.error() << '\n';
    return PublishResult::skipped;
  }
  Json message{{"type", "data"}, {"value", snapshot_json(*snapshot)}};
  if (expanded_transition) {
    message["transitions"] = {{"expanded", *expanded_transition}};
  }
  return write_json(message)
             ? PublishResult::published
             : PublishResult::output_failed;
}

[[nodiscard]] int poll_timeout() {
  const auto now = time_point_cast<milliseconds>(system_clock::now());
  const auto timeout = gisland::until_next_minute(now).count();
  return static_cast<int>(std::clamp<std::int64_t>(timeout, 0, INT_MAX));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] int run() {
  std::optional<gisland::ClockCalendarModel> model;
  bool content_transitions = false;
  int protocol_minor = 1;
  std::string input;
  std::array<char, 4096> chunk{};

  while (true) {
    pollfd descriptor{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    const int ready = ::poll(&descriptor, 1, model ? poll_timeout() : -1);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "clock-calendar: poll failed\n";
      return EXIT_FAILURE;
    }
    if (ready == 0) {
      if (model &&
          publish(*model, floor<seconds>(system_clock::now())) == PublishResult::output_failed) {
        return EXIT_FAILURE;
      }
      continue;
    }
    if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
      std::cerr << "clock-calendar: input polling failed\n";
      return EXIT_FAILURE;
    }
    if ((descriptor.revents & POLLHUP) != 0 && (descriptor.revents & POLLIN) == 0) {
      return EXIT_SUCCESS;
    }
    if ((descriptor.revents & POLLIN) == 0) {
      continue;
    }

    const ssize_t count = ::read(STDIN_FILENO, chunk.data(), chunk.size());
    if (count == 0) {
      return EXIT_SUCCESS;
    }
    if (count < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      std::cerr << "clock-calendar: input read failed\n";
      return EXIT_FAILURE;
    }
    input.append(chunk.data(), static_cast<std::size_t>(count));
    if (input.size() > maximum_line_size && !input.contains('\n')) {
      std::cerr << "clock-calendar: input line exceeds one MiB\n";
      return EXIT_FAILURE;
    }

    std::size_t newline = 0;
    while ((newline = input.find('\n')) != std::string::npos) {
      if (newline > maximum_line_size) {
        std::cerr << "clock-calendar: input line exceeds one MiB\n";
        return EXIT_FAILURE;
      }
      std::string line = input.substr(0, newline);
      input.erase(0, newline + 1);
      if (line.ends_with('\r')) {
        line.pop_back();
      }
      const Json message = Json::parse(line, nullptr, false);
      if (message.is_discarded()) {
        std::cerr << "clock-calendar: malformed JSON message\n";
        if (!model) {
          return EXIT_FAILURE;
        }
        continue;
      }

      if (!model) {
        auto options = parse_init(message);
        if (!options) {
          std::cerr << "clock-calendar: " << options.error() << '\n';
          return EXIT_FAILURE;
        }
        content_transitions = options->content_transitions;
        protocol_minor = options->protocol_minor;
        auto created = gisland::ClockCalendarModel::create(std::move(options->options),
                                                           floor<seconds>(system_clock::now()));
        if (!created) {
          std::cerr << "clock-calendar: " << created.error() << '\n';
          return EXIT_FAILURE;
        }
        model.emplace(std::move(*created));
        if (!write_json({{"type", "ready"},
                         {"protocol_major", 1},
                         {"protocol_minor", protocol_minor},
                         {"capabilities", content_transitions
                                              ? Json{"data-snapshots", "content-transitions"}
                                              : Json{"data-snapshots"}}}) ||
            publish(*model, floor<seconds>(system_clock::now())) != PublishResult::published) {
          return EXIT_FAILURE;
        }
        continue;
      }

      if (!message.is_object()) {
        std::cerr << "clock-calendar: expected an object message\n";
        continue;
      }
      const std::string type = message.value("type", "");
      if (type == "shutdown") {
        return EXIT_SUCCESS;
      }
      if (type == "visibility") {
        continue;
      }
      if (type != "action" || !message.contains("action_id") ||
          !message.at("action_id").is_string()) {
        std::cerr << "clock-calendar: unsupported core message\n";
        continue;
      }

      const std::string action_id = message.at("action_id").get<std::string>();
      const auto accepted = model->apply_action(action_id, floor<seconds>(system_clock::now()));
      if (!accepted) {
        std::cerr << "clock-calendar: " << accepted.error() << '\n';
        continue;
      }
      if (!write_json(
              {{"type", "action_result"}, {"action_id", action_id}, {"accepted", *accepted}})) {
        return EXIT_FAILURE;
      }
      std::optional<std::string_view> transition;
      if (content_transitions && action_id == "next-month") {
        transition = "slide-left";
      } else if (content_transitions && action_id == "previous-month") {
        transition = "slide-right";
      }
      if (*accepted && publish(*model, floor<seconds>(system_clock::now()), transition) ==
                           PublishResult::output_failed) {
        return EXIT_FAILURE;
      }
    }
  }
}

} // namespace

int main() {
  try {
    return run();
  } catch (const std::exception &error) {
    std::cerr << "clock-calendar: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
