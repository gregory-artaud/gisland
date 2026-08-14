#pragma once

#include <nlohmann/json_fwd.hpp>

#include <poll.h>

#include <array>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace gisland {

enum class LuaTransportErrorCode {
  invalid_descriptor,
  poll_failed,
  read_failed,
  write_failed,
  input_eof,
  embedded_nul,
  record_too_large,
  malformed_json,
  non_object,
  queue_overflow,
  callback_failed,
};

struct LuaTransportError {
  LuaTransportErrorCode code;
  std::string message;
};

class LuaTransport {
public:
  static constexpr std::size_t max_record_bytes = std::size_t{8} * 1024U * 1024U;
  static constexpr std::size_t max_output_messages = 256;
  // Host outbound records include 8 MiB publishes; the core-to-module WriteQueue is 1 MiB.
  static constexpr std::size_t max_output_bytes = std::size_t{16} * 1024U * 1024U;

  using Result = std::expected<void, LuaTransportError>;
  using Emit = std::function<Result(nlohmann::json)>;
  using RecordCallback = std::function<Result(const nlohmann::json &, const Emit &)>;

  [[nodiscard]] static std::expected<std::unique_ptr<LuaTransport>, LuaTransportError>
  create(int input_fd, int output_fd, RecordCallback callback);

  ~LuaTransport();
  LuaTransport(const LuaTransport &) = delete;
  LuaTransport &operator=(const LuaTransport &) = delete;
  LuaTransport(LuaTransport &&) noexcept;
  LuaTransport &operator=(LuaTransport &&) noexcept;

  [[nodiscard]] Result send(nlohmann::json record);
  [[nodiscard]] std::array<pollfd, 2> poll_descriptors(bool read_enabled = true) const;
  [[nodiscard]] Result advance(std::span<const pollfd> descriptors);
  [[nodiscard]] Result poll_once(int timeout_ms, bool read_enabled = true);
  [[nodiscard]] Result run();

  [[nodiscard]] std::size_t pending_output_messages() const noexcept;
  [[nodiscard]] std::size_t pending_output_bytes() const noexcept;
  [[nodiscard]] bool has_buffered_input() const noexcept;

private:
  class Impl;
  explicit LuaTransport(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

} // namespace gisland
