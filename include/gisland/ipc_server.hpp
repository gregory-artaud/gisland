#pragma once

#include "gisland/context.hpp"
#include "gisland/control.hpp"

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace gisland {

struct IpcServerError {
  std::string message;
};

using ControlHandler = std::function<ControlDispatchResult(const ControlCommand &)>;
using PendingControlCancellation = std::function<void(PendingControlToken)>;

class IpcServer final {
public:
  static constexpr std::size_t maximum_clients = 16;
  static constexpr std::size_t maximum_pending_clients = 8;

  [[nodiscard]] static std::expected<IpcServer, IpcServerError>
  create(std::string_view runtime_directory);

  IpcServer(IpcServer &&) noexcept;
  IpcServer &operator=(IpcServer &&) noexcept;
  IpcServer(const IpcServer &) = delete;
  IpcServer &operator=(const IpcServer &) = delete;
  ~IpcServer();

  void advance(MonotonicTime now, const ControlHandler &handler,
               const PendingControlCancellation &on_cancel = {});
  [[nodiscard]] bool complete(PendingControlToken token, const ControlResponse &response,
                              MonotonicTime now);
  [[nodiscard]] const std::string &socket_path() const noexcept;
  [[nodiscard]] std::size_t client_count() const noexcept;
  [[nodiscard]] std::size_t pending_count() const noexcept;

private:
  class Implementation;
  explicit IpcServer(std::unique_ptr<Implementation> implementation);

  std::unique_ptr<Implementation> implementation_;
};

} // namespace gisland
