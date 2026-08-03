#pragma once

#include "gisland/bootstrap.hpp"

#include <string>

namespace gisland {

struct ApplicationConfig {
  std::string title{"gisland"};
  int target_fps{60};
  int top_margin{8};
};

class Application {
public:
  explicit Application(RuntimeBootstrap bootstrap, ApplicationConfig config = {});

  [[nodiscard]] int run();

private:
  RuntimeBootstrap bootstrap_;
  ApplicationConfig config_;
};

} // namespace gisland
