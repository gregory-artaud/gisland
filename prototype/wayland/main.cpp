#include "wayland_prototype_options.hpp"
#include "wayland_prototype_runtime.hpp"

#include <iostream>
#include <span>
#include <string_view>
#include <vector>

int main(int argc, char **argv) {
  std::vector<std::string_view> arguments;
  arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  auto options = gisland::wayland_prototype::parse_options(arguments);
  if (!options) {
    std::cerr << options.error() << '\n';
    return 2;
  }
  return gisland::wayland_prototype::run(*options);
}
