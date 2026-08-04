#pragma once

#include "gisland/context.hpp"
#include "gisland/control.hpp"

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace gisland {

struct IpcServerError {
  std::string message;
};

using ControlHandler = std::function<ControlResponse(const ControlCommand &)>;

class IpcServer final {
public:
  static constexpr std::size_t maximum_clients = 16;

  [[nodiscard]] static std::expected<IpcServer, IpcServerError>
  create(std::string_view runtime_directory);

  IpcServer(IpcServer &&) noexcept;
  IpcServer &operator=(IpcServer &&) noexcept;
  IpcServer(const IpcServer &) = delete;
  IpcServer &operator=(const IpcServer &) = delete;
  ~IpcServer();

  void advance(MonotonicTime now, const ControlHandler &handler);
  [[nodiscard]] const std::string &socket_path() const noexcept;
  [[nodiscard]] std::size_t client_count() const noexcept;

private:
  class Implementation;
  explicit IpcServer(std::unique_ptr<Implementation> implementation);

  std::unique_ptr<Implementation> implementation_;
};

} // namespace gisland
