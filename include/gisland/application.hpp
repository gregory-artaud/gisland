#pragma once

#include <string>

namespace gisland {

struct ApplicationConfig {
  int width{640};
  int height{360};
  std::string title{"gisland"};
  int target_fps{60};
};

class Application {
public:
  explicit Application(ApplicationConfig config = {});

  [[nodiscard]] int run();

private:
  ApplicationConfig config_;
};

} // namespace gisland
