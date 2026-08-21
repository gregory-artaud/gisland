#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace gisland::wayland_prototype {

struct Options {
  bool automated{false};
  int width{420};
  int height{220};
  int top_margin{8};
};

[[nodiscard]] std::expected<Options, std::string>
parse_options(std::span<const std::string_view> arguments);

} // namespace gisland::wayland_prototype
