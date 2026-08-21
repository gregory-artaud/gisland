#include "wayland_prototype_options.hpp"

#include <charconv>

namespace gisland::wayland_prototype {
namespace {

std::expected<int, std::string> parse_integer(std::string_view name, std::string_view value,
                                              int minimum, int maximum) {
  int parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() || parsed < minimum ||
      parsed > maximum) {
    return std::unexpected(std::string{name} + " is out of range");
  }
  return parsed;
}

} // namespace

std::expected<Options, std::string> parse_options(std::span<const std::string_view> arguments) {
  Options options;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (argument == "--automated") {
      options.automated = true;
      continue;
    }
    if (argument != "--width" && argument != "--height" && argument != "--top-margin") {
      return std::unexpected("unknown argument: " + std::string{argument});
    }
    if (++index == arguments.size()) {
      return std::unexpected("missing value for " + std::string{argument});
    }
    const int minimum = argument == "--top-margin" ? 0 : 41;
    const int maximum = argument == "--top-margin" ? 4096 : 8192;
    auto value = parse_integer(argument, arguments[index], minimum, maximum);
    if (!value) {
      return std::unexpected(value.error());
    }
    if (argument == "--width") {
      options.width = *value;
    } else if (argument == "--height") {
      options.height = *value;
    } else {
      options.top_margin = *value;
    }
  }
  return options;
}

} // namespace gisland::wayland_prototype
