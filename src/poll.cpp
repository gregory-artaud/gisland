#include "gisland/poll.hpp"

#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>

namespace gisland {

std::expected<void, std::string> poll_with_timeout(std::span<pollfd> descriptors, int timeout_ms) {
  if (timeout_ms < -1) {
    return std::unexpected("invalid poll timeout");
  }

  using Clock = std::chrono::steady_clock;
  const std::optional<Clock::time_point> deadline =
      timeout_ms >= 0 ? std::optional{Clock::now() + std::chrono::milliseconds{timeout_ms}}
                      : std::nullopt;
  int remaining_ms = timeout_ms;
  while (::poll(descriptors.data(), descriptors.size(), remaining_ms) < 0) {
    if (errno != EINTR) {
      return std::unexpected(std::string{"poll failed: "} + std::strerror(errno));
    }
    if (!deadline) {
      continue;
    }

    const auto now = Clock::now();
    if (now >= *deadline) {
      remaining_ms = 0;
      continue;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(*deadline - now).count();
    remaining_ms =
        static_cast<int>(std::min<std::int64_t>(remaining, std::numeric_limits<int>::max()));
  }
  return {};
}

} // namespace gisland
