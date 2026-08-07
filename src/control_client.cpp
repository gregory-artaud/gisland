#include "gisland/control_client.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <variant>

namespace gisland {
namespace {

constexpr std::size_t maximum_response_bytes = (64U * 1024U) + 1U;

[[nodiscard]] std::unexpected<ControlClientError> client_error(std::string message) {
  return std::unexpected(ControlClientError{std::move(message)});
}

[[nodiscard]] std::unexpected<ControlClientError> system_error(std::string operation) {
  return client_error(std::move(operation) + ": " + std::strerror(errno));
}

[[nodiscard]] std::expected<void, ControlClientError>
wait_for(int descriptor, short events, std::chrono::steady_clock::time_point deadline) {
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return client_error("control socket phase timed out");
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    const auto bounded = std::min<std::int64_t>(remaining.count(), std::numeric_limits<int>::max());
    pollfd candidate{.fd = descriptor, .events = events, .revents = 0};
    const int ready = ::poll(&candidate, 1, static_cast<int>(bounded));
    if (ready > 0) {
      if ((candidate.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
          (candidate.revents & events) == 0) {
        return client_error("control socket closed during I/O");
      }
      return {};
    }
    if (ready == 0) {
      return client_error("control socket phase timed out");
    }
    if (errno != EINTR) {
      return system_error("control socket poll failed");
    }
  }
}

[[nodiscard]] std::string human_modules(const std::vector<ModuleControlStatus> &modules) {
  std::ostringstream output;
  output << "modules:\n";
  for (const auto &module : modules) {
    output << module.id << '\t' << control_module_state_name(module.state) << '\t'
           << (module.available ? "available" : "unavailable") << '\n';
  }
  return output.str();
}

} // namespace

std::expected<ControlResponse, ControlClientError>
send_control_command(std::string_view socket_path, const ControlCommand &command,
                     std::chrono::milliseconds phase_timeout) {
  if (socket_path.empty() || socket_path.size() >= sizeof(sockaddr_un::sun_path) ||
      phase_timeout <= std::chrono::milliseconds::zero()) {
    return client_error("invalid control socket client configuration");
  }
  const int descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (descriptor < 0) {
    return system_error("cannot create control socket");
  }
  const auto close_with = [descriptor](std::unexpected<ControlClientError> error) {
    static_cast<void>(::close(descriptor));
    return std::expected<ControlResponse, ControlClientError>{std::move(error)};
  };

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path.data(), socket_path.size());
  address.sun_path[socket_path.size()] = '\0';
  const auto address_length =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + socket_path.size() + 1U);
  if (::connect(descriptor, reinterpret_cast<const sockaddr *>(&address), address_length) != 0) {
    if (errno != EINPROGRESS) {
      return close_with(system_error("cannot connect to gisland"));
    }
    auto ready = wait_for(descriptor, POLLOUT, std::chrono::steady_clock::now() + phase_timeout);
    if (!ready) {
      return close_with(std::unexpected(ready.error()));
    }
    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) != 0) {
      return close_with(system_error("cannot inspect control connection"));
    }
    if (socket_error != 0) {
      errno = socket_error;
      return close_with(system_error("cannot connect to gisland"));
    }
  }

  const std::string request = serialize_control_request(command);
  std::size_t offset = 0;
  const auto write_deadline = std::chrono::steady_clock::now() + phase_timeout;
  while (offset < request.size()) {
    const ssize_t sent =
        ::send(descriptor, request.data() + offset, request.size() - offset, MSG_NOSIGNAL);
    if (sent > 0) {
      offset += static_cast<std::size_t>(sent);
      continue;
    }
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      return close_with(system_error("cannot write control request"));
    }
    auto ready = wait_for(descriptor, POLLOUT, write_deadline);
    if (!ready) {
      return close_with(std::unexpected(ready.error()));
    }
  }
  if (::shutdown(descriptor, SHUT_WR) != 0) {
    return close_with(system_error("cannot finish control request"));
  }

  std::string record;
  const auto read_deadline = std::chrono::steady_clock::now() + phase_timeout;
  while (true) {
    char buffer[4096];
    const ssize_t received = ::recv(descriptor, buffer, sizeof(buffer), 0);
    if (received > 0) {
      record.append(buffer, static_cast<std::size_t>(received));
      if (record.size() > maximum_response_bytes) {
        return close_with(client_error("control response exceeds 64 KiB"));
      }
      continue;
    }
    if (received == 0) {
      break;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      return close_with(system_error("cannot read control response"));
    }
    auto ready = wait_for(descriptor, POLLIN, read_deadline);
    if (!ready) {
      return close_with(std::unexpected(ready.error()));
    }
  }
  static_cast<void>(::close(descriptor));

  const auto newline = record.find('\n');
  if (newline == std::string::npos || newline + 1U != record.size() ||
      record.find('\n', newline + 1U) != std::string::npos) {
    return client_error("control response is not one JSONL record");
  }
  std::string_view line{record.data(), newline};
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  auto response = parse_control_response(line);
  if (!response) {
    return client_error(response.error());
  }
  return std::move(*response);
}

std::string format_control_output(const ControlResponse &response, bool json_output) {
  if (json_output) {
    return serialize_control_result(response);
  }
  return std::visit(
      [](const auto &typed) -> std::string {
        using Type = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Type, EmptyControlResult>) {
          return "ok\n";
        } else if constexpr (std::is_same_v<Type, ControlError>) {
          return std::string{control_error_code_name(typed.code)} + ": " + typed.message + '\n';
        } else if constexpr (std::is_same_v<Type, ModulesStatus>) {
          return human_modules(typed.modules);
        } else {
          std::ostringstream output;
          output << "mode: " << (typed.mode == IslandMode::expanded ? "expanded" : "compact")
                 << '\n';
          const auto write_slot = [&output](std::string_view name,
                                            const std::optional<ActiveContextStatus> &context) {
            output << name << ": ";
            if (context) {
              output << context->instance_id << '/' << context->context_id << " (priority "
                     << context->priority << ")\n";
            } else {
              output << "none\n";
            }
          };
          write_slot("compact", typed.compact);
          write_slot("expanded", typed.expanded);
          output << "socket: " << typed.socket << '\n' << human_modules(typed.modules);
          return output.str();
        }
      },
      response.value());
}

} // namespace gisland
