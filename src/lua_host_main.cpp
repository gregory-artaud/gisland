#include "gisland/lua_host.hpp"
#include "gisland/lua_transport.hpp"

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] gisland::LuaHostError transport_error(const gisland::LuaTransportError &error) {
  return {gisland::LuaHostErrorCode::output_error, error.message};
}

[[nodiscard]] gisland::LuaTransportError host_error(const gisland::LuaHostError &error) {
  return {gisland::LuaTransportErrorCode::callback_failed, error.message};
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "gisland-lua-host: expected one Lua entry path\n";
    return EXIT_FAILURE;
  }
  std::signal(SIGPIPE, SIG_IGN);

  auto host = gisland::LuaHost::load(argv[1]);
  if (!host) {
    std::cerr << "gisland-lua-host: " << host.error().message << '\n';
    return EXIT_FAILURE;
  }

  auto transport = gisland::LuaTransport::create(
      STDIN_FILENO, STDOUT_FILENO,
      [&host](const nlohmann::json &record,
              const gisland::LuaTransport::Emit &emit) -> gisland::LuaTransport::Result {
        const auto result = (*host)->handle(
            record,
            [&emit](nlohmann::json output) -> std::expected<void, gisland::LuaHostError> {
              auto sent = emit(std::move(output));
              if (!sent) {
                return std::unexpected(transport_error(sent.error()));
              }
              return {};
            },
            gisland::LuaHost::Clock::now());
        if (!result) {
          return std::unexpected(host_error(result.error()));
        }
        return {};
      });
  if (!transport) {
    std::cerr << "gisland-lua-host: " << transport.error().message << '\n';
    return EXIT_FAILURE;
  }

  const auto emit =
      [&transport](nlohmann::json output) -> std::expected<void, gisland::LuaHostError> {
    auto sent = (*transport)->send(std::move(output));
    if (!sent) {
      return std::unexpected(transport_error(sent.error()));
    }
    return {};
  };

  while (!(*host)->stopped()) {
    const int timeout = (*transport)->pending_output_messages() > 0
                            ? -1
                            : gisland::lua_host_poll_timeout((*host)->next_deadline(),
                                                             gisland::LuaHost::Clock::now());
    auto result = (*transport)->poll_once(timeout);
    if (!result) {
      std::cerr << "gisland-lua-host: " << result.error().message << '\n';
      return EXIT_FAILURE;
    }
    if ((*transport)->pending_output_messages() > 0) {
      continue;
    }
    auto due = (*host)->run_due(gisland::LuaHost::Clock::now(), emit);
    if (!due) {
      std::cerr << "gisland-lua-host: " << due.error().message << '\n';
      return EXIT_FAILURE;
    }
  }
  while ((*transport)->pending_output_messages() > 0) {
    const auto result = (*transport)->poll_once(-1);
    if (!result) {
      std::cerr << "gisland-lua-host: " << result.error().message << '\n';
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}
