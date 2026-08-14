#pragma once

#include <poll.h>

#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace gisland {

struct GlibPollQuery {
  std::vector<pollfd> descriptors;
  int timeout_ms{-1};
};

class GlibMainContext {
public:
  GlibMainContext();
  ~GlibMainContext();
  GlibMainContext(const GlibMainContext &) = delete;
  GlibMainContext &operator=(const GlibMainContext &) = delete;
  GlibMainContext(GlibMainContext &&) noexcept;
  GlibMainContext &operator=(GlibMainContext &&) noexcept;

  [[nodiscard]] std::expected<GlibPollQuery, std::string> prepare();
  [[nodiscard]] std::expected<void, std::string>
  check_and_dispatch(std::span<const pollfd> descriptors);
  void cancel_poll();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace gisland
