#include "gisland/control_client.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

int main(int argc, char **argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(std::max(argc - 1, 0)));
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  const auto invocation = gisland::parse_control_arguments(arguments);
  if (!invocation) {
    std::cerr << invocation.error() << '\n';
    return EXIT_FAILURE;
  }
  const char *runtime_directory = std::getenv("XDG_RUNTIME_DIR");
  if (runtime_directory == nullptr || *runtime_directory == '\0') {
    std::cerr << "XDG_RUNTIME_DIR is unset\n";
    return EXIT_FAILURE;
  }
  const auto response = gisland::send_control_command(
      std::string{runtime_directory} + "/gisland.sock", invocation->command);
  if (!response) {
    std::cerr << response.error().message << '\n';
    return EXIT_FAILURE;
  }
  if (std::holds_alternative<gisland::ControlError>(response->value())) {
    std::cerr << gisland::format_control_output(*response, false);
    return EXIT_FAILURE;
  }
  std::cout << gisland::format_control_output(*response, invocation->json_output);
  return EXIT_SUCCESS;
}
