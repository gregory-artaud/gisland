#include "gisland/content_fingerprint.hpp"
#include "gisland/glib_main_context.hpp"
#include "gisland/lua_host.hpp"
#include "gisland/lua_transport.hpp"
#include "gisland/poll.hpp"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <linux/openat2.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] gisland::LuaHostError transport_error(const gisland::LuaTransportError &error) {
  return {gisland::LuaHostErrorCode::output_error, error.message};
}

[[nodiscard]] gisland::LuaTransportError host_error(const gisland::LuaHostError &error) {
  return {gisland::LuaTransportErrorCode::callback_failed, error.message};
}

[[nodiscard]] std::expected<void, std::string> set_host_bindir() {
  std::error_code error;
  const auto executable = std::filesystem::canonical("/proc/self/exe", error);
  if (error) {
    return std::unexpected("cannot resolve the host executable directory");
  }
  const auto bindir = executable.parent_path().string();
  if (::setenv("GISLAND_LUA_HOST_BINDIR", bindir.c_str(), 1) != 0) {
    return std::unexpected("cannot export the host executable directory");
  }
  return {};
}

[[nodiscard]] std::expected<std::string, std::string>
read_expected_entry(const std::filesystem::path &path, std::string_view expected) {
  const int directory =
      ::open(path.parent_path().c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0) {
    return std::unexpected("module package directory is unavailable");
  }
  const open_how how{.flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW,
                     .mode = 0,
                     .resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS};
  const auto name = path.filename().string();
  const int descriptor =
      static_cast<int>(::syscall(SYS_openat2, directory, name.c_str(), &how, sizeof(how)));
  ::close(directory);
  if (descriptor < 0) {
    return std::unexpected("module entry is unavailable");
  }
  struct stat metadata{};
  const std::string actual_identity =
      ::fstat(descriptor, &metadata) == 0
          ? std::to_string(static_cast<std::uint64_t>(metadata.st_dev)) + ':' +
                std::to_string(static_cast<std::uint64_t>(metadata.st_ino)) + ':' +
                std::to_string(static_cast<std::uint64_t>(metadata.st_size)) + ':' +
                std::to_string(metadata.st_mtim.tv_sec) + ':' +
                std::to_string(metadata.st_mtim.tv_nsec)
          : std::string{};
  if (!expected.empty() && actual_identity != expected) {
    ::close(descriptor);
    return std::unexpected("module entry changed before launch");
  }
  std::string source;
  std::array<char, 64 * 1024> buffer{};
  while (true) {
    const auto count = ::read(descriptor, buffer.data(), buffer.size());
    if (count > 0) {
      source.append(buffer.data(), static_cast<std::size_t>(count));
      continue;
    }
    if (count == 0) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    ::close(descriptor);
    return std::unexpected("module entry cannot be read");
  }
  ::close(descriptor);
  return source;
}

constexpr std::string_view fingerprint_argument_prefix = "--gisland-entry-fingerprint=";

[[nodiscard]] int minimum_timeout(int left, int right) {
  if (left < 0) {
    return right;
  }
  if (right < 0) {
    return left;
  }
  return std::min(left, right);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "gisland-lua-host: expected a Lua entry path\n";
    return EXIT_FAILURE;
  }
  std::signal(SIGPIPE, SIG_IGN);
  if (const auto bindir = set_host_bindir(); !bindir) {
    std::cerr << "gisland-lua-host: " << bindir.error() << '\n';
    return EXIT_FAILURE;
  }

  std::expected<std::unique_ptr<gisland::LuaHost>, gisland::LuaHostError> host;
  const std::string_view fingerprint_argument = argc >= 3 ? argv[2] : "";
  if (fingerprint_argument.starts_with(fingerprint_argument_prefix)) {
    const auto expected_fingerprint =
        fingerprint_argument.substr(fingerprint_argument_prefix.size());
    if (expected_fingerprint.size() != 16 ||
        expected_fingerprint.find_first_not_of("0123456789abcdef") != std::string_view::npos) {
      std::cerr << "gisland-lua-host: invalid entry fingerprint\n";
      return EXIT_FAILURE;
    }
    const char *expected_identity = std::getenv("GISLAND_LUA_ENTRY_IDENTITY");
    auto source =
        read_expected_entry(argv[1], expected_identity != nullptr ? expected_identity : "");
    if (!source) {
      std::cerr << "gisland-lua-host: " << source.error() << '\n';
      return EXIT_FAILURE;
    }
    if (gisland::content_fingerprint(*source) != expected_fingerprint) {
      std::cerr << "gisland-lua-host: module entry content changed before launch\n";
      return EXIT_FAILURE;
    }
    host = gisland::LuaHost::load(argv[1], std::move(*source));
  } else {
    host = gisland::LuaHost::load(argv[1]);
  }
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

  gisland::GlibMainContext glib_context;

  while (!(*host)->stopped()) {
    const bool output_pending = (*transport)->pending_output_messages() > 0;
    auto transport_descriptors = (*transport)->poll_descriptors(!output_pending);
    std::vector<pollfd> descriptors{transport_descriptors.begin(), transport_descriptors.end()};
    std::expected<gisland::GlibPollQuery, std::string> glib_query;
    int timeout = -1;
    if (!output_pending) {
      timeout =
          gisland::lua_host_poll_timeout((*host)->next_deadline(), gisland::LuaHost::Clock::now());
      glib_query = glib_context.prepare();
      if (!glib_query) {
        std::cerr << "gisland-lua-host: " << glib_query.error() << '\n';
        return EXIT_FAILURE;
      }
      timeout = minimum_timeout(timeout, glib_query->timeout_ms);
      if ((*transport)->has_buffered_input()) {
        timeout = 0;
      }
      descriptors.insert(descriptors.end(), glib_query->descriptors.begin(),
                         glib_query->descriptors.end());
    }

    auto polled = gisland::poll_with_timeout(descriptors, timeout);
    if (!polled) {
      std::cerr << "gisland-lua-host: " << polled.error() << '\n';
      return EXIT_FAILURE;
    }
    auto result = (*transport)->advance(std::span{descriptors}.first(transport_descriptors.size()));
    if (!result) {
      std::cerr << "gisland-lua-host: " << result.error().message << '\n';
      return EXIT_FAILURE;
    }
    if ((*transport)->pending_output_messages() > 0) {
      if (!output_pending) {
        glib_context.cancel_poll();
      }
      continue;
    }
    if (output_pending) {
      continue;
    }
    std::expected<void, std::string> dispatched;
    auto callbacks = (*host)->run_external_callbacks(gisland::LuaHost::Clock::now(), emit, [&] {
      dispatched = glib_context.check_and_dispatch(
          std::span{descriptors}.subspan(transport_descriptors.size()));
    });
    if (!dispatched) {
      std::cerr << "gisland-lua-host: " << dispatched.error() << '\n';
      return EXIT_FAILURE;
    }
    if (!callbacks) {
      std::cerr << "gisland-lua-host: " << callbacks.error().message << '\n';
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
    const auto result = (*transport)->poll_once(-1, false);
    if (!result) {
      std::cerr << "gisland-lua-host: " << result.error().message << '\n';
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}
