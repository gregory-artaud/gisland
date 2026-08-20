#pragma once

#include <poll.h>

#include <expected>
#include <span>
#include <string>

namespace gisland {

[[nodiscard]] std::expected<void, std::string> poll_with_timeout(std::span<pollfd> descriptors,
                                                                 int timeout_ms);

} // namespace gisland
