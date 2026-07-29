#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gisland {

struct BufferedLine {
  std::string text;
  bool truncated;
};

enum class LineBufferErrorCode { embedded_nul, line_too_long, finished };

struct LineBufferError {
  LineBufferErrorCode code;
};

class LineBuffer {
public:
  static constexpr std::size_t default_protocol_limit = std::size_t{1024} * 1024U;
  static constexpr std::size_t default_stderr_limit = std::size_t{64} * 1024U;

  [[nodiscard]] static LineBuffer protocol(std::size_t maximum_line_bytes = default_protocol_limit);
  [[nodiscard]] static LineBuffer
  standard_error(std::size_t maximum_line_bytes = default_stderr_limit);

  [[nodiscard]] std::expected<std::vector<BufferedLine>, LineBufferError>
  append(std::span<const std::byte> bytes);
  [[nodiscard]] std::expected<std::optional<BufferedLine>, LineBufferError> finish();

private:
  LineBuffer(std::size_t maximum_line_bytes, bool truncate, bool reject_nul);

  [[nodiscard]] BufferedLine take_line();

  std::size_t maximum_line_bytes_;
  bool truncate_;
  bool reject_nul_;
  bool truncated_{false};
  bool finished_{false};
  std::string current_;
};

} // namespace gisland
