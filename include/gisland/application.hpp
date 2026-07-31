#pragma once

#include <string>

namespace gisland {

struct ApplicationConfig {
  std::string title{"gisland"};
  int target_fps{60};
  std::string monitor{"primary"};
  int top_margin{8};
};

class Application {
public:
  explicit Application(ApplicationConfig config = {});

  [[nodiscard]] int run();

private:
  ApplicationConfig config_;
};

} // namespace gisland
