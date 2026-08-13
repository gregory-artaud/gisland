#pragma once

#include "gisland/platform_host.hpp"

#include <expected>

namespace gisland {

[[nodiscard]] std::expected<PlatformHostPtr, PlatformError>
create_x11_platform_host(void *native_window_handle);

} // namespace gisland
