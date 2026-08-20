#include "gisland/glib_main_context.hpp"

#include <glib.h>

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace gisland {

class GlibMainContext::Impl {
public:
  Impl() : context_(g_main_context_ref(g_main_context_default())) {}

  ~Impl() {
    release();
    g_main_context_unref(context_);
  }

  [[nodiscard]] std::expected<GlibPollQuery, std::string> prepare() {
    if (acquired_) {
      return std::unexpected("GLib main context poll is already prepared");
    }
    if (g_main_context_acquire(context_) == 0) {
      return std::unexpected("cannot acquire the default GLib main context");
    }
    acquired_ = true;

    const gboolean ready = g_main_context_prepare(context_, &priority_);
    gint timeout = -1;
    poll_descriptors_.clear();
    while (true) {
      const auto capacity = static_cast<gint>(poll_descriptors_.size());
      const gint required =
          g_main_context_query(context_, priority_, &timeout, poll_descriptors_.data(), capacity);
      if (required <= capacity) {
        poll_descriptors_.resize(static_cast<std::size_t>(required));
        break;
      }
      poll_descriptors_.resize(static_cast<std::size_t>(required));
    }

    GlibPollQuery query;
    query.timeout_ms = ready != 0 ? 0 : timeout;
    query.descriptors.reserve(poll_descriptors_.size());
    for (const auto &descriptor : poll_descriptors_) {
      query.descriptors.push_back(
          {.fd = descriptor.fd, .events = static_cast<short>(descriptor.events), .revents = 0});
    }
    return query;
  }

  [[nodiscard]] std::expected<void, std::string>
  check_and_dispatch(std::span<const pollfd> descriptors) {
    if (!acquired_) {
      return std::unexpected("GLib main context poll was not prepared");
    }
    if (descriptors.size() != poll_descriptors_.size()) {
      release();
      return std::unexpected("GLib poll descriptor set changed unexpectedly");
    }
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
      if (descriptors[index].fd != poll_descriptors_[index].fd) {
        release();
        return std::unexpected("GLib poll descriptor set changed unexpectedly");
      }
      poll_descriptors_[index].revents = static_cast<gushort>(descriptors[index].revents);
    }

    const gboolean ready = g_main_context_check(context_, priority_, poll_descriptors_.data(),
                                                static_cast<gint>(poll_descriptors_.size()));
    if (ready != 0) {
      g_main_context_dispatch(context_);
    }
    release();
    return {};
  }

  void cancel_poll() { release(); }

private:
  void release() {
    if (acquired_) {
      g_main_context_release(context_);
      acquired_ = false;
    }
  }

  GMainContext *context_;
  std::vector<GPollFD> poll_descriptors_;
  gint priority_{0};
  bool acquired_{false};
};

GlibMainContext::GlibMainContext() : impl_(std::make_unique<Impl>()) {}
GlibMainContext::~GlibMainContext() = default;
GlibMainContext::GlibMainContext(GlibMainContext &&) noexcept = default;
GlibMainContext &GlibMainContext::operator=(GlibMainContext &&) noexcept = default;

std::expected<GlibPollQuery, std::string> GlibMainContext::prepare() { return impl_->prepare(); }

std::expected<void, std::string>
GlibMainContext::check_and_dispatch(std::span<const pollfd> descriptors) {
  return impl_->check_and_dispatch(descriptors);
}

void GlibMainContext::cancel_poll() { impl_->cancel_poll(); }

} // namespace gisland
