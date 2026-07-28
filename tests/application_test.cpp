#include "gisland/application.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return false;
  }
  return true;
}

} // namespace

int main() {
  const gisland::ApplicationConfig config;
  bool passed = true;

  passed &= expect(config.width == 640, "default width must be 640");
  passed &= expect(config.height == 360, "default height must be 360");
  passed &= expect(config.title == "gisland", "default title must be gisland");
  passed &= expect(config.target_fps == 60, "default frame rate must be 60");

  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
