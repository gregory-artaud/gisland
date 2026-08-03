#include "gisland/application.hpp"
#include "gisland/bootstrap.hpp"

#include <cstdlib>
#include <iostream>
#include <utility>

int main() {
  auto bootstrap = gisland::load_runtime_bootstrap_from_environment();
  if (!bootstrap) {
    std::cerr << bootstrap.error().path << ": " << bootstrap.error().message << '\n';
    return EXIT_FAILURE;
  }
  gisland::Application application{std::move(*bootstrap)};
  return application.run();
}
