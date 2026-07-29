#include "gisland/write_queue.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <utility>

namespace gisland {

WriteQueue::WriteQueue(std::size_t maximum_messages, std::size_t maximum_bytes)
    : maximum_messages_(maximum_messages), maximum_bytes_(maximum_bytes) {}

std::expected<void, WriteQueueError> WriteQueue::push(std::string record) {
  if (record.empty() || !record.ends_with('\n') || record.find('\n') != record.size() - 1 ||
      record.contains('\0')) {
    return std::unexpected(WriteQueueError::invalid_record);
  }
  if (records_.size() >= maximum_messages_) {
    return std::unexpected(WriteQueueError::message_limit);
  }
  if (record.size() > maximum_bytes_ - pending_bytes_) {
    return std::unexpected(WriteQueueError::byte_limit);
  }

  pending_bytes_ += record.size();
  records_.push_back(std::move(record));
  return {};
}

std::span<const char> WriteQueue::front_span() const noexcept {
  if (records_.empty()) {
    return {};
  }
  const auto &front = records_.front();
  return {front.data() + front_offset_, front.size() - front_offset_};
}

std::expected<void, WriteQueueError> WriteQueue::consume(std::size_t byte_count) {
  const auto front = front_span();
  if (byte_count > front.size()) {
    return std::unexpected(WriteQueueError::invalid_consume);
  }
  if (records_.empty()) {
    return {};
  }

  front_offset_ += byte_count;
  pending_bytes_ -= byte_count;
  if (front_offset_ == records_.front().size()) {
    records_.pop_front();
    front_offset_ = 0;
  }
  return {};
}

bool WriteQueue::wants_write() const noexcept { return !records_.empty(); }

std::size_t WriteQueue::message_count() const noexcept { return records_.size(); }

std::size_t WriteQueue::pending_bytes() const noexcept { return pending_bytes_; }

} // namespace gisland
