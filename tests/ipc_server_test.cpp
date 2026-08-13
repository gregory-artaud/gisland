#include "gisland/ipc_server.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;

namespace {

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    std::array<char, 64> pattern{};
    const std::string source = "/tmp/gisland-ipc-test-XXXXXX";
    std::ranges::copy(source, pattern.begin());
    path_ = ::mkdtemp(pattern.data());
  }

  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

  [[nodiscard]] const std::string &path() const { return path_; }

private:
  std::string path_;
};

int connect_client(const std::string &path) {
  const int descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  REQUIRE(descriptor >= 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  REQUIRE(path.size() < sizeof(address.sun_path));
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  REQUIRE(::connect(descriptor, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) ==
          0);
  return descriptor;
}

void send_all(int descriptor, std::string_view value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const ssize_t sent =
        ::send(descriptor, value.data() + offset, value.size() - offset, MSG_NOSIGNAL);
    REQUIRE(sent > 0);
    offset += static_cast<std::size_t>(sent);
  }
}

std::string receive_record(int descriptor) {
  std::string result;
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t received = ::recv(descriptor, buffer.data(), buffer.size(), 0);
    if (received == 0) {
      return result;
    }
    REQUIRE(received > 0);
    result.append(buffer.data(), static_cast<std::size_t>(received));
  }
}

std::string exchange(gisland::IpcServer &server, std::string_view request, int &dispatches) {
  const int client = connect_client(server.socket_path());
  send_all(client, request);
  REQUIRE(::shutdown(client, SHUT_WR) == 0);
  const auto handler = [&dispatches](const gisland::ControlCommand &) {
    ++dispatches;
    return gisland::ControlResponse{};
  };
  server.advance(gisland::MonotonicTime{}, handler);
  server.advance(gisland::MonotonicTime{}, handler);
  std::string response = receive_record(client);
  REQUIRE(::close(client) == 0);
  return response;
}

mode_t permissions(const std::string &path) {
  struct stat status{};
  REQUIRE(::lstat(path.c_str(), &status) == 0);
  return status.st_mode & 0777;
}

} // namespace

TEST_CASE("IPC server owns a locked private runtime socket and safely cleans it") {
  TemporaryDirectory directory;
  const std::string socket_path = directory.path() + "/gisland.sock";
  const std::string lock_path = directory.path() + "/gisland.lock";

  {
    auto server = gisland::IpcServer::create(directory.path());
    REQUIRE(server.has_value());
    CHECK(server->socket_path() == socket_path);
    CHECK(permissions(socket_path) == 0600);
    CHECK(permissions(lock_path) == 0600);
    CHECK_FALSE(gisland::IpcServer::create(directory.path()).has_value());
  }
  CHECK_FALSE(std::filesystem::exists(socket_path));
  CHECK(std::filesystem::exists(lock_path));
}

TEST_CASE("IPC server replaces only stale same-user sockets") {
  TemporaryDirectory stale_directory;
  const std::string stale_path = stale_directory.path() + "/gisland.sock";
  const int stale = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  REQUIRE(stale >= 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, stale_path.c_str(), stale_path.size() + 1);
  REQUIRE(::bind(stale, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
  REQUIRE(::close(stale) == 0);
  REQUIRE(gisland::IpcServer::create(stale_directory.path()).has_value());

  TemporaryDirectory unsafe_directory;
  const std::string unsafe_path = unsafe_directory.path() + "/gisland.sock";
  const int file = ::open(unsafe_path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
  REQUIRE(file >= 0);
  REQUIRE(::close(file) == 0);
  CHECK_FALSE(gisland::IpcServer::create(unsafe_directory.path()).has_value());
  CHECK(std::filesystem::is_regular_file(unsafe_path));
}

TEST_CASE("IPC server cleanup preserves a replacement pathname entry") {
  TemporaryDirectory directory;
  const std::string socket_path = directory.path() + "/gisland.sock";
  {
    auto server = gisland::IpcServer::create(directory.path());
    REQUIRE(server.has_value());
    REQUIRE(::unlink(socket_path.c_str()) == 0);
    const int replacement = ::open(socket_path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    REQUIRE(replacement >= 0);
    REQUIRE(::close(replacement) == 0);
  }
  CHECK(std::filesystem::is_regular_file(socket_path));
}

TEST_CASE("IPC server dispatches one request only after write-half closure") {
  TemporaryDirectory directory;
  auto server = gisland::IpcServer::create(directory.path());
  REQUIRE(server.has_value());
  const int client = connect_client(server->socket_path());
  int dispatches = 0;
  const auto handler = [&dispatches](const gisland::ControlCommand &) {
    ++dispatches;
    return gisland::ControlResponse{};
  };
  const auto now = gisland::MonotonicTime{};

  send_all(client, R"({"version":1,"command":"open"})"
                   "\n");
  server->advance(now, handler);
  CHECK(dispatches == 0);
  REQUIRE(::shutdown(client, SHUT_WR) == 0);
  server->advance(now, handler);
  CHECK(dispatches == 1);
  const auto response = nlohmann::json::parse(receive_record(client));
  CHECK(response.at("ok") == true);
  REQUIRE(::close(client) == 0);
}

TEST_CASE("IPC server rejects duplicate records without dispatching") {
  TemporaryDirectory directory;
  auto server = gisland::IpcServer::create(directory.path());
  REQUIRE(server.has_value());
  const int client = connect_client(server->socket_path());
  send_all(client, R"({"version":1,"command":"open"})"
                   "\n"
                   R"({"version":1,"command":"close"})"
                   "\n");
  REQUIRE(::shutdown(client, SHUT_WR) == 0);
  int dispatches = 0;
  server->advance(gisland::MonotonicTime{}, [&dispatches](const gisland::ControlCommand &) {
    ++dispatches;
    return gisland::ControlResponse{};
  });
  server->advance(gisland::MonotonicTime{}, [&dispatches](const gisland::ControlCommand &) {
    ++dispatches;
    return gisland::ControlResponse{};
  });
  const auto response = nlohmann::json::parse(receive_record(client));
  CHECK(dispatches == 0);
  CHECK(response.at("ok") == false);
  CHECK(response.at("error").at("code") == "invalid_request");
  REQUIRE(::close(client) == 0);
}

TEST_CASE("IPC server validates exact JSONL framing before dispatch") {
  TemporaryDirectory directory;
  auto server = gisland::IpcServer::create(directory.path());
  REQUIRE(server.has_value());

  SECTION("CRLF is accepted") {
    int dispatches = 0;
    const auto response = nlohmann::json::parse(exchange(*server,
                                                         R"({"version":1,"command":"open"})"
                                                         "\r\n",
                                                         dispatches));
    CHECK(dispatches == 1);
    CHECK(response.at("ok") == true);
  }

  for (const std::string &request : {
           std::string{},
           std::string{R"({"version":1,"command":"open"})"},
           std::string{"\n"},
           std::string{R"({"version":1,"command":"open"})"
                       "\ntrailing"},
           std::string{"{\"version\":1,\"command\":\"open\",\"command\":\"close\"}\n"},
       }) {
    CAPTURE(request);
    int dispatches = 0;
    const std::string record = exchange(*server, request, dispatches);
    CHECK(dispatches == 0);
    if (request.empty()) {
      CHECK(record.empty());
    } else {
      const auto response = nlohmann::json::parse(record);
      CHECK(response.at("ok") == false);
      CHECK(response.at("error").at("code") == "invalid_request");
    }
  }
}

TEST_CASE("IPC server caps accepted clients at sixteen") {
  TemporaryDirectory directory;
  auto server = gisland::IpcServer::create(directory.path());
  REQUIRE(server.has_value());
  std::vector<int> clients;
  for (std::size_t index = 0; index < gisland::IpcServer::maximum_clients + 1U; ++index) {
    clients.push_back(connect_client(server->socket_path()));
  }
  const auto handler = [](const gisland::ControlCommand &) { return gisland::ControlResponse{}; };
  for (int frame = 0; frame < 5; ++frame) {
    server->advance(gisland::MonotonicTime{}, handler);
  }
  CHECK(server->client_count() == gisland::IpcServer::maximum_clients);
  for (const int client : clients) {
    static_cast<void>(::close(client));
  }
  server->advance(gisland::MonotonicTime{}, handler);
}

TEST_CASE("IPC server enforces accept budgets and request deadlines") {
  TemporaryDirectory directory;
  auto server = gisland::IpcServer::create(directory.path());
  REQUIRE(server.has_value());
  std::vector<int> clients;
  for (int index = 0; index < 8; ++index) {
    clients.push_back(connect_client(server->socket_path()));
  }
  const auto handler = [](const gisland::ControlCommand &) { return gisland::ControlResponse{}; };
  const auto now = gisland::MonotonicTime{};
  server->advance(now, handler);
  CHECK(server->client_count() == 4);
  server->advance(now, handler);
  CHECK(server->client_count() == 8);
  server->advance(now + 2s, handler);
  CHECK(server->client_count() == 0);
  for (const int client : clients) {
    CHECK(::recv(client, nullptr, 0, 0) == 0);
    REQUIRE(::close(client) == 0);
  }
}

TEST_CASE("IPC server holds deferred requests while serving later clients") {
  TemporaryDirectory directory;
  auto server = gisland::IpcServer::create(directory.path());
  REQUIRE(server.has_value());
  const int pending_client = connect_client(server->socket_path());
  const int immediate_client = connect_client(server->socket_path());
  send_all(pending_client, R"({"version":1,"command":"open"})"
                           "\n");
  send_all(immediate_client, R"({"version":1,"command":"close"})"
                             "\n");
  REQUIRE(::shutdown(pending_client, SHUT_WR) == 0);
  REQUIRE(::shutdown(immediate_client, SHUT_WR) == 0);

  const gisland::PendingControlToken token{41};
  const auto handler =
      [token](const gisland::ControlCommand &command) -> gisland::ControlDispatchResult {
    if (std::holds_alternative<gisland::OpenControl>(command)) {
      return token;
    }
    return gisland::ControlResponse{};
  };
  server->advance(gisland::MonotonicTime{}, handler);
  server->advance(gisland::MonotonicTime{}, handler);

  CHECK(nlohmann::json::parse(receive_record(immediate_client)).at("ok") == true);
  CHECK(server->pending_count() == 1);
  CHECK(server->complete(token, gisland::ControlResponse{}, gisland::MonotonicTime{}));
  CHECK_FALSE(server->complete(token, gisland::ControlResponse{}, gisland::MonotonicTime{}));
  server->advance(gisland::MonotonicTime{}, handler);
  CHECK(nlohmann::json::parse(receive_record(pending_client)).at("ok") == true);
  REQUIRE(::close(immediate_client) == 0);
  REQUIRE(::close(pending_client) == 0);
}

TEST_CASE("IPC server reports deferred ownership cancellation on disconnect and deadline") {
  TemporaryDirectory directory;
  auto server = gisland::IpcServer::create(directory.path());
  REQUIRE(server.has_value());
  std::vector<gisland::PendingControlToken> cancelled;
  const auto handler = [](const gisland::ControlCommand &) -> gisland::ControlDispatchResult {
    return gisland::PendingControlToken{73};
  };
  const auto on_cancel = [&cancelled](gisland::PendingControlToken token) {
    cancelled.push_back(token);
  };

  const int disconnected = connect_client(server->socket_path());
  send_all(disconnected, R"({"version":1,"command":"open"})"
                         "\n");
  REQUIRE(::shutdown(disconnected, SHUT_WR) == 0);
  server->advance(gisland::MonotonicTime{}, handler, on_cancel);
  server->advance(gisland::MonotonicTime{}, handler, on_cancel);
  REQUIRE(::close(disconnected) == 0);
  server->advance(gisland::MonotonicTime{}, handler, on_cancel);
  REQUIRE(cancelled == std::vector{gisland::PendingControlToken{73}});

  const int timed_out = connect_client(server->socket_path());
  send_all(timed_out, R"({"version":1,"command":"open"})"
                      "\n");
  REQUIRE(::shutdown(timed_out, SHUT_WR) == 0);
  server->advance(gisland::MonotonicTime{}, handler, on_cancel);
  server->advance(gisland::MonotonicTime{}, handler, on_cancel);
  server->advance(gisland::MonotonicTime{} + 3s, handler, on_cancel);
  CHECK(cancelled ==
        std::vector{gisland::PendingControlToken{73}, gisland::PendingControlToken{73}});
  REQUIRE(::close(timed_out) == 0);
}

TEST_CASE("IPC server bounds deferred clients") {
  TemporaryDirectory directory;
  auto server = gisland::IpcServer::create(directory.path());
  REQUIRE(server.has_value());
  std::vector<int> clients;
  for (std::size_t index = 0; index < gisland::IpcServer::maximum_pending_clients + 1U; ++index) {
    clients.push_back(connect_client(server->socket_path()));
    send_all(clients.back(), R"({"version":1,"command":"open"})"
                             "\n");
    REQUIRE(::shutdown(clients.back(), SHUT_WR) == 0);
  }
  std::uint64_t next_token = 1;
  std::vector<gisland::PendingControlToken> cancelled;
  const auto handler =
      [&next_token](const gisland::ControlCommand &) -> gisland::ControlDispatchResult {
    return gisland::PendingControlToken{next_token++};
  };
  for (int frame = 0; frame < 8; ++frame) {
    server->advance(
        gisland::MonotonicTime{}, handler,
        [&cancelled](gisland::PendingControlToken token) { cancelled.push_back(token); });
  }
  CHECK(server->pending_count() == gisland::IpcServer::maximum_pending_clients);
  REQUIRE(cancelled.size() == 1);
  CHECK(cancelled.front().value == gisland::IpcServer::maximum_pending_clients + 1U);
  for (const int client : clients) {
    static_cast<void>(::close(client));
  }
}

TEST_CASE("IPC server bounds deferred responses and expires blocked writes") {
  TemporaryDirectory directory;
  auto server = gisland::IpcServer::create(directory.path());
  REQUIRE(server.has_value());
  const int client = connect_client(server->socket_path());
  send_all(client, R"({"version":1,"command":"open"})"
                   "\n");
  REQUIRE(::shutdown(client, SHUT_WR) == 0);
  const gisland::PendingControlToken token{91};
  const auto handler = [token](const gisland::ControlCommand &) -> gisland::ControlDispatchResult {
    return token;
  };
  server->advance(gisland::MonotonicTime{}, handler);
  server->advance(gisland::MonotonicTime{}, handler);
  REQUIRE(server->complete(
      token,
      gisland::ControlResponse{gisland::ControlError{gisland::ControlErrorCode::internal_error,
                                                     std::string(70U * 1024U, 'x')}},
      gisland::MonotonicTime{}));
  server->advance(gisland::MonotonicTime{} + 2s, handler);
  CHECK(server->client_count() == 0);
  REQUIRE(::close(client) == 0);
}
