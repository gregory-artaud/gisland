#include "gisland/lua_transport.hpp"

#include "gisland/line_buffer.hpp"
#include "gisland/write_queue.hpp"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <utility>

namespace gisland {
namespace {

[[nodiscard]] LuaTransportError error(LuaTransportErrorCode code, std::string message) {
  return {code, std::move(message)};
}

[[nodiscard]] std::expected<void, LuaTransportError> make_nonblocking(int descriptor) {
  const int flags = ::fcntl(descriptor, F_GETFL);
  if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
    return std::unexpected(
        error(LuaTransportErrorCode::invalid_descriptor, "cannot configure transport descriptor"));
  }
  return {};
}

[[nodiscard]] LuaTransportError line_error(LineBufferErrorCode code) {
  switch (code) {
  case LineBufferErrorCode::embedded_nul:
    return error(LuaTransportErrorCode::embedded_nul, "input record contains NUL");
  case LineBufferErrorCode::line_too_long:
    return error(LuaTransportErrorCode::record_too_large, "input record exceeds 8 MiB");
  case LineBufferErrorCode::finished:
    return error(LuaTransportErrorCode::input_eof, "input is already closed");
  }
  return error(LuaTransportErrorCode::read_failed, "input framing failed");
}

} // namespace

class LuaTransport::Impl {
public:
  Impl(int input_fd, int output_fd, RecordCallback callback)
      : input_fd_(input_fd), output_fd_(output_fd), callback_(std::move(callback)),
        input_(LineBuffer::protocol(max_record_bytes)),
        output_(max_output_messages, max_output_bytes) {}

  [[nodiscard]] Result send(nlohmann::json record) {
    if (!record.is_object()) {
      return std::unexpected(
          error(LuaTransportErrorCode::non_object, "output protocol record must be a JSON object"));
    }

    std::string serialized;
    try {
      serialized = record.dump();
    } catch (const nlohmann::json::exception &) {
      return std::unexpected(
          error(LuaTransportErrorCode::malformed_json, "output record cannot be serialized"));
    }
    if (serialized.size() > max_record_bytes) {
      return std::unexpected(
          error(LuaTransportErrorCode::record_too_large, "output record exceeds 8 MiB"));
    }
    serialized.push_back('\n');

    const auto queued = output_.push(std::move(serialized));
    if (!queued) {
      return std::unexpected(
          error(LuaTransportErrorCode::queue_overflow, "output queue limit exceeded"));
    }
    return {};
  }

  [[nodiscard]] Result poll_once(int timeout_ms) {
    std::array<pollfd, 2> descriptors{{
        {.fd = input_fd_, .events = POLLIN, .revents = 0},
        {.fd = output_fd_,
         .events = static_cast<short>(output_.wants_write() ? POLLOUT : 0),
         .revents = 0},
    }};
    int ready = 0;
    do {
      ready = ::poll(descriptors.data(), descriptors.size(), timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready < 0) {
      return std::unexpected(error(LuaTransportErrorCode::poll_failed,
                                   std::string{"poll failed: "} + std::strerror(errno)));
    }

    if ((descriptors[1].revents & POLLOUT) != 0) {
      auto written = write_output();
      if (!written) {
        return written;
      }
    }
    if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      return std::unexpected(
          error(LuaTransportErrorCode::write_failed, "standard output is unavailable"));
    }
    if ((descriptors[0].revents & (POLLIN | POLLHUP)) != 0) {
      auto read = read_input();
      if (!read) {
        return read;
      }
    }
    if ((descriptors[0].revents & (POLLERR | POLLNVAL)) != 0) {
      return std::unexpected(error(LuaTransportErrorCode::read_failed, "standard input failed"));
    }
    return {};
  }

  [[nodiscard]] Result run() {
    while (true) {
      auto result = poll_once(-1);
      if (!result) {
        return result;
      }
    }
  }

  [[nodiscard]] std::size_t pending_output_messages() const noexcept {
    return output_.message_count();
  }

  [[nodiscard]] std::size_t pending_output_bytes() const noexcept {
    return output_.pending_bytes();
  }

private:
  [[nodiscard]] Result read_input() {
    std::array<std::byte, 64 * 1024> buffer{};
    while (true) {
      const auto count = ::read(input_fd_, buffer.data(), buffer.size());
      if (count > 0) {
        const auto lines = input_.append(
            std::span<const std::byte>{buffer.data(), static_cast<std::size_t>(count)});
        if (!lines) {
          return std::unexpected(line_error(lines.error().code));
        }
        for (const auto &line : *lines) {
          auto handled = handle_line(line.text);
          if (!handled) {
            return handled;
          }
        }
        continue;
      }
      if (count == 0) {
        const auto trailing = input_.finish();
        if (!trailing) {
          return std::unexpected(line_error(trailing.error().code));
        }
        if (trailing->has_value()) {
          auto handled = handle_line(trailing->value().text);
          if (!handled) {
            return handled;
          }
        }
        return std::unexpected(error(LuaTransportErrorCode::input_eof, "standard input closed"));
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return {};
      }
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(error(LuaTransportErrorCode::read_failed,
                                   std::string{"read failed: "} + std::strerror(errno)));
    }
  }

  [[nodiscard]] Result handle_line(const std::string &line) {
    nlohmann::json record;
    try {
      record = nlohmann::json::parse(line);
    } catch (const nlohmann::json::parse_error &) {
      return std::unexpected(
          error(LuaTransportErrorCode::malformed_json, "input record is malformed JSON"));
    }
    if (!record.is_object()) {
      return std::unexpected(
          error(LuaTransportErrorCode::non_object, "input protocol record must be a JSON object"));
    }
    auto result =
        callback_(record, [this](nlohmann::json emitted) { return send(std::move(emitted)); });
    if (!result && result.error().code != LuaTransportErrorCode::queue_overflow &&
        result.error().code != LuaTransportErrorCode::record_too_large) {
      return std::unexpected(error(LuaTransportErrorCode::callback_failed, result.error().message));
    }
    return result;
  }

  [[nodiscard]] Result write_output() {
    while (output_.wants_write()) {
      const auto front = output_.front_span();
      const auto count = ::write(output_fd_, front.data(), front.size());
      if (count > 0) {
        const auto consumed = output_.consume(static_cast<std::size_t>(count));
        if (!consumed) {
          return std::unexpected(
              error(LuaTransportErrorCode::write_failed, "output queue accounting failed"));
        }
        continue;
      }
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return {};
      }
      if (count < 0 && errno == EINTR) {
        continue;
      }
      return std::unexpected(error(LuaTransportErrorCode::write_failed,
                                   std::string{"write failed: "} + std::strerror(errno)));
    }
    return {};
  }

  int input_fd_;
  int output_fd_;
  RecordCallback callback_;
  LineBuffer input_;
  WriteQueue output_;
};

std::expected<std::unique_ptr<LuaTransport>, LuaTransportError>
LuaTransport::create(int input_fd, int output_fd, RecordCallback callback) {
  if (input_fd < 0 || output_fd < 0 || !callback) {
    return std::unexpected(
        error(LuaTransportErrorCode::invalid_descriptor, "invalid transport configuration"));
  }
  auto input_result = make_nonblocking(input_fd);
  if (!input_result) {
    return std::unexpected(input_result.error());
  }
  auto output_result = make_nonblocking(output_fd);
  if (!output_result) {
    return std::unexpected(output_result.error());
  }
  return std::unique_ptr<LuaTransport>{
      new LuaTransport{std::make_unique<Impl>(input_fd, output_fd, std::move(callback))}};
}

LuaTransport::LuaTransport(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LuaTransport::~LuaTransport() = default;
LuaTransport::LuaTransport(LuaTransport &&) noexcept = default;
LuaTransport &LuaTransport::operator=(LuaTransport &&) noexcept = default;

LuaTransport::Result LuaTransport::send(nlohmann::json record) {
  return impl_->send(std::move(record));
}

LuaTransport::Result LuaTransport::poll_once(int timeout_ms) {
  return impl_->poll_once(timeout_ms);
}

LuaTransport::Result LuaTransport::run() { return impl_->run(); }

std::size_t LuaTransport::pending_output_messages() const noexcept {
  return impl_->pending_output_messages();
}

std::size_t LuaTransport::pending_output_bytes() const noexcept {
  return impl_->pending_output_bytes();
}

} // namespace gisland
