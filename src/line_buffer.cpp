#include "gisland/line_buffer.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace gisland {

LineBuffer LineBuffer::protocol(std::size_t maximum_line_bytes) {
  return LineBuffer{maximum_line_bytes, false, true};
}

LineBuffer LineBuffer::standard_error(std::size_t maximum_line_bytes) {
  return LineBuffer{maximum_line_bytes, true, false};
}

LineBuffer::LineBuffer(std::size_t maximum_line_bytes, bool truncate, bool reject_nul)
    : maximum_line_bytes_(maximum_line_bytes), truncate_(truncate), reject_nul_(reject_nul) {
  current_.reserve(maximum_line_bytes);
}

std::expected<std::vector<BufferedLine>, LineBufferError>
LineBuffer::append(std::span<const std::byte> bytes) {
  if (finished_) {
    return std::unexpected(LineBufferError{LineBufferErrorCode::finished});
  }

  std::vector<BufferedLine> lines;
  for (const auto byte : bytes) {
    const auto character = static_cast<char>(byte);
    if (character == '\0' && reject_nul_) {
      return std::unexpected(LineBufferError{LineBufferErrorCode::embedded_nul});
    }
    if (character == '\n') {
      lines.push_back(take_line());
      continue;
    }
    if (current_.size() < maximum_line_bytes_) {
      current_.push_back(character);
      continue;
    }
    if (!truncate_) {
      return std::unexpected(LineBufferError{LineBufferErrorCode::line_too_long});
    }
    truncated_ = true;
  }
  return lines;
}

std::expected<std::optional<BufferedLine>, LineBufferError> LineBuffer::finish() {
  if (finished_) {
    return std::optional<BufferedLine>{};
  }
  finished_ = true;
  if (current_.empty() && !truncated_) {
    return std::optional<BufferedLine>{};
  }
  return std::optional<BufferedLine>{take_line()};
}

BufferedLine LineBuffer::take_line() {
  if (!current_.empty() && current_.back() == '\r') {
    current_.pop_back();
  }
  BufferedLine line{std::move(current_), truncated_};
  current_.clear();
  current_.reserve(maximum_line_bytes_);
  truncated_ = false;
  return line;
}

} // namespace gisland
