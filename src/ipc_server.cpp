#include "gisland/ipc_server.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace gisland {
namespace {

constexpr std::size_t maximum_record_bytes = 64U * 1024U;
constexpr std::size_t syscall_bytes = 4096;
constexpr std::size_t frame_io_bytes = 64U * 1024U;
constexpr int frame_accepts = 4;
constexpr auto phase_timeout = std::chrono::seconds{2};

[[nodiscard]] IpcServerError system_error(std::string operation) {
  return IpcServerError{std::move(operation) + ": " + std::strerror(errno)};
}

[[nodiscard]] socklen_t address_size(const std::string &path) {
  return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1U);
}

[[nodiscard]] ControlResponse invalid_request(std::string message) {
  return ControlResponse{ControlError{ControlErrorCode::invalid_request, std::move(message)}};
}

} // namespace

class IpcServer::Implementation final {
public:
  struct Client {
    int descriptor{-1};
    std::uint64_t sequence{};
    MonotonicTime accepted_at;
    std::string input;
    std::optional<std::string> response;
    std::size_t response_offset{};
    std::optional<MonotonicTime> response_deadline;
    bool close{false};
  };

  Implementation(int lock_descriptor, int listen_descriptor, std::string socket_path, dev_t device,
                 ino_t inode)
      : lock_descriptor_(lock_descriptor), listen_descriptor_(listen_descriptor),
        socket_path_(std::move(socket_path)), socket_device_(device), socket_inode_(inode) {}

  ~Implementation() {
    for (auto &client : clients_) {
      static_cast<void>(::close(client.descriptor));
    }
    if (listen_descriptor_ >= 0) {
      static_cast<void>(::close(listen_descriptor_));
    }
    struct stat status{};
    if (::lstat(socket_path_.c_str(), &status) == 0 && status.st_dev == socket_device_ &&
        status.st_ino == socket_inode_) {
      static_cast<void>(::unlink(socket_path_.c_str()));
    }
    if (lock_descriptor_ >= 0) {
      static_cast<void>(::close(lock_descriptor_));
    }
  }

  void advance(MonotonicTime now, const ControlHandler &handler) {
    accept_clients(now);
    std::size_t remaining_io = frame_io_bytes;
    const std::size_t count = clients_.size();
    if (count == 0) {
      return;
    }

    std::size_t start = 0;
    while (start < count && clients_[start].sequence <= last_serviced_sequence_) {
      ++start;
    }
    if (start == count) {
      start = 0;
    }
    for (std::size_t visited = 0; visited < count; ++visited) {
      auto &client = clients_[(start + visited) % count];
      if (service(client, now, handler, remaining_io)) {
        last_serviced_sequence_ = client.sequence;
      }
    }

    std::erase_if(clients_, [](Client &client) {
      if (!client.close) {
        return false;
      }
      static_cast<void>(::close(client.descriptor));
      return true;
    });
  }

  [[nodiscard]] const std::string &socket_path() const noexcept { return socket_path_; }
  [[nodiscard]] std::size_t client_count() const noexcept { return clients_.size(); }

private:
  void accept_clients(MonotonicTime now) {
    for (int accepted = 0; accepted < frame_accepts; ++accepted) {
      const int descriptor =
          ::accept4(listen_descriptor_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (descriptor < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;
        }
        if (errno == EINTR) {
          --accepted;
          continue;
        }
        return;
      }

      ucred credentials{};
      socklen_t size = sizeof(credentials);
      const bool trusted =
          ::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 &&
          size == sizeof(credentials) && credentials.uid == ::geteuid();
      if (!trusted || clients_.size() >= IpcServer::maximum_clients) {
        static_cast<void>(::close(descriptor));
        continue;
      }
      clients_.push_back(Client{.descriptor = descriptor,
                                .sequence = next_client_sequence_++,
                                .accepted_at = now,
                                .input = {},
                                .response = std::nullopt,
                                .response_offset = 0,
                                .response_deadline = std::nullopt,
                                .close = false});
    }
  }

  [[nodiscard]] bool service(Client &client, MonotonicTime now, const ControlHandler &handler,
                             std::size_t &remaining_io) {
    if ((!client.response && now >= client.accepted_at + phase_timeout) ||
        (client.response_deadline && now >= *client.response_deadline)) {
      client.close = true;
      return true;
    }

    bool serviced = false;
    if (!client.response && remaining_io > 0) {
      read(client, now, handler, remaining_io);
      serviced = true;
    }
    if (client.response && !client.close && remaining_io > 0) {
      write(client, remaining_io);
      serviced = true;
    }
    return serviced;
  }

  void read(Client &client, MonotonicTime now, const ControlHandler &handler,
            std::size_t &remaining_io) {
    std::array<char, syscall_bytes> buffer{};
    const std::size_t capacity = std::min(buffer.size(), remaining_io);
    const ssize_t received = ::recv(client.descriptor, buffer.data(), capacity, 0);
    if (received > 0) {
      const auto bytes = static_cast<std::size_t>(received);
      remaining_io -= bytes;
      client.input.append(buffer.data(), bytes);
      if (client.input.size() > maximum_record_bytes + 1U) {
        prepare_response(client, invalid_request("control request exceeds 64 KiB"), now);
      }
      return;
    }
    if (received == 0) {
      if (client.input.empty()) {
        client.close = true;
        return;
      }
      prepare_request(client, now, handler);
      return;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      client.close = true;
    }
  }

  void prepare_request(Client &client, MonotonicTime now, const ControlHandler &handler) {
    const auto newline = client.input.find('\n');
    if (newline == std::string::npos || newline + 1U != client.input.size() ||
        client.input.find('\n', newline + 1U) != std::string::npos ||
        newline > maximum_record_bytes) {
      prepare_response(client, invalid_request("control connection must contain one JSONL record"),
                       now);
      return;
    }
    std::string_view line{client.input.data(), newline};
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.empty()) {
      prepare_response(client, invalid_request("control request record is empty"), now);
      return;
    }

    const auto command = parse_control_request(line);
    if (!command) {
      prepare_response(client, ControlResponse{command.error()}, now);
      return;
    }
    prepare_response(client, handler(*command), now);
  }

  static void prepare_response(Client &client, const ControlResponse &response, MonotonicTime now) {
    std::string record = serialize_control_response(response);
    if (record.size() > maximum_record_bytes + 1U) {
      record = serialize_control_response(ControlResponse{
          ControlError{ControlErrorCode::internal_error, "control response exceeds 64 KiB"}});
    }
    client.response = std::move(record);
    client.response_deadline = now + phase_timeout;
  }

  static void write(Client &client, std::size_t &remaining_io) {
    const std::string &response = client.response.value();
    const std::string_view pending{response.data() + client.response_offset,
                                   response.size() - client.response_offset};
    const std::size_t size = std::min({pending.size(), syscall_bytes, remaining_io});
    const ssize_t sent = ::send(client.descriptor, pending.data(), size, MSG_NOSIGNAL);
    if (sent > 0) {
      const auto bytes = static_cast<std::size_t>(sent);
      remaining_io -= bytes;
      client.response_offset += bytes;
      if (client.response_offset == response.size()) {
        client.close = true;
      }
      return;
    }
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      client.close = true;
    }
  }

  int lock_descriptor_;
  int listen_descriptor_;
  std::string socket_path_;
  dev_t socket_device_;
  ino_t socket_inode_;
  std::uint64_t next_client_sequence_{1};
  std::uint64_t last_serviced_sequence_{};
  std::vector<Client> clients_;
};

std::expected<IpcServer, IpcServerError> IpcServer::create(std::string_view runtime_directory) {
  if (runtime_directory.empty()) {
    return std::unexpected(IpcServerError{"XDG_RUNTIME_DIR is empty"});
  }
  const std::string directory{runtime_directory};
  struct stat directory_status{};
  if (::stat(directory.c_str(), &directory_status) != 0) {
    return std::unexpected(system_error("cannot inspect XDG_RUNTIME_DIR"));
  }
  if (!S_ISDIR(directory_status.st_mode) || directory_status.st_uid != ::geteuid() ||
      ::access(directory.c_str(), R_OK | W_OK | X_OK) != 0) {
    return std::unexpected(IpcServerError{"XDG_RUNTIME_DIR is not a usable owned directory"});
  }

  const std::string lock_path = directory + "/gisland.lock";
  const std::string socket_path = directory + "/gisland.sock";
  if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
    return std::unexpected(IpcServerError{"control socket path is too long"});
  }

  const int lock_descriptor =
      ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (lock_descriptor < 0) {
    return std::unexpected(system_error("cannot open runtime lock"));
  }
  const auto fail_locked = [lock_descriptor](IpcServerError error) {
    static_cast<void>(::close(lock_descriptor));
    return std::expected<IpcServer, IpcServerError>{std::unexpected(std::move(error))};
  };
  struct stat lock_status{};
  if (::fstat(lock_descriptor, &lock_status) != 0) {
    return fail_locked(system_error("cannot inspect runtime lock"));
  }
  if (!S_ISREG(lock_status.st_mode) || lock_status.st_uid != ::geteuid()) {
    return fail_locked(IpcServerError{"runtime lock is not a regular owned file"});
  }
  if (::fchmod(lock_descriptor, 0600) != 0) {
    return fail_locked(system_error("cannot secure runtime lock"));
  }
  if (::flock(lock_descriptor, LOCK_EX | LOCK_NB) != 0) {
    return fail_locked(system_error("another gisland instance holds the runtime lock"));
  }

  struct stat existing{};
  if (::lstat(socket_path.c_str(), &existing) == 0) {
    if (!S_ISSOCK(existing.st_mode) || existing.st_uid != ::geteuid()) {
      return fail_locked(IpcServerError{"existing control path is not a same-user socket"});
    }
    if (::unlink(socket_path.c_str()) != 0) {
      return fail_locked(system_error("cannot remove stale control socket"));
    }
  } else if (errno != ENOENT) {
    return fail_locked(system_error("cannot inspect control socket path"));
  }

  const int listen_descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listen_descriptor < 0) {
    return fail_locked(system_error("cannot create control socket"));
  }
  const auto fail_socket = [lock_descriptor, listen_descriptor,
                            &socket_path](IpcServerError error) {
    static_cast<void>(::close(listen_descriptor));
    static_cast<void>(::unlink(socket_path.c_str()));
    static_cast<void>(::close(lock_descriptor));
    return std::expected<IpcServer, IpcServerError>{std::unexpected(std::move(error))};
  };

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1U);
  if (::bind(listen_descriptor, reinterpret_cast<const sockaddr *>(&address),
             address_size(socket_path)) != 0) {
    return fail_socket(system_error("cannot bind control socket"));
  }
  if (::chmod(socket_path.c_str(), 0600) != 0) {
    return fail_socket(system_error("cannot secure control socket"));
  }
  if (::listen(listen_descriptor, 32) != 0) {
    return fail_socket(system_error("cannot listen on control socket"));
  }
  struct stat socket_status{};
  if (::lstat(socket_path.c_str(), &socket_status) != 0 || !S_ISSOCK(socket_status.st_mode) ||
      socket_status.st_uid != ::geteuid()) {
    return fail_socket(IpcServerError{"cannot verify bound control socket"});
  }

  return IpcServer{std::make_unique<Implementation>(lock_descriptor, listen_descriptor, socket_path,
                                                    socket_status.st_dev, socket_status.st_ino)};
}

IpcServer::IpcServer(std::unique_ptr<Implementation> implementation)
    : implementation_(std::move(implementation)) {}
IpcServer::IpcServer(IpcServer &&) noexcept = default;
IpcServer &IpcServer::operator=(IpcServer &&) noexcept = default;
IpcServer::~IpcServer() = default;

void IpcServer::advance(MonotonicTime now, const ControlHandler &handler) {
  implementation_->advance(now, handler);
}

const std::string &IpcServer::socket_path() const noexcept {
  return implementation_->socket_path();
}

std::size_t IpcServer::client_count() const noexcept { return implementation_->client_count(); }

} // namespace gisland
