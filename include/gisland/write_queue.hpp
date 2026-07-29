#pragma once

#include <cstddef>
#include <deque>
#include <expected>
#include <span>
#include <string>

namespace gisland {

enum class WriteQueueError { invalid_record, message_limit, byte_limit, invalid_consume };

class WriteQueue {
public:
  static constexpr std::size_t default_message_limit = 256;
  static constexpr std::size_t default_byte_limit = 1024U * 1024U;

  explicit WriteQueue(std::size_t maximum_messages = default_message_limit,
                      std::size_t maximum_bytes = default_byte_limit);

  [[nodiscard]] std::expected<void, WriteQueueError> push(std::string record);
  [[nodiscard]] std::span<const char> front_span() const noexcept;
  [[nodiscard]] std::expected<void, WriteQueueError> consume(std::size_t byte_count);

  [[nodiscard]] bool wants_write() const noexcept;
  [[nodiscard]] std::size_t message_count() const noexcept;
  [[nodiscard]] std::size_t pending_bytes() const noexcept;

private:
  std::size_t maximum_messages_;
  std::size_t maximum_bytes_;
  std::deque<std::string> records_;
  std::size_t front_offset_{0};
  std::size_t pending_bytes_{0};
};

} // namespace gisland
