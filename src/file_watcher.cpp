#include "gisland/file_watcher.hpp"

#include <sys/inotify.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <utility>

namespace gisland {
namespace {

constexpr std::uint32_t watch_mask = IN_CLOSE_WRITE | IN_ATTRIB | IN_CREATE | IN_DELETE |
                                     IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;

[[nodiscard]] std::string system_error(std::string_view operation) {
  return std::string{operation} + ": " + std::strerror(errno);
}

} // namespace

ReloadDebouncer::ReloadDebouncer(std::chrono::steady_clock::duration quiet_period)
    : quiet_period_{quiet_period} {}

void ReloadDebouncer::observe(std::chrono::steady_clock::time_point now) {
  deadline_ = now + quiet_period_;
}

bool ReloadDebouncer::consume_due(std::chrono::steady_clock::time_point now) {
  if (!deadline_ || now < *deadline_) {
    return false;
  }
  deadline_.reset();
  return true;
}

void ReloadDebouncer::clear() { deadline_.reset(); }

bool ReloadDebouncer::pending() const { return deadline_.has_value(); }

FileWatcher::FileWatcher(int descriptor) : descriptor_{descriptor} {}

FileWatcher::FileWatcher(FileWatcher &&other) noexcept
    : descriptor_{std::exchange(other.descriptor_, -1)},
      directories_{std::move(other.directories_)} {}

FileWatcher &FileWatcher::operator=(FileWatcher &&other) noexcept {
  if (this != &other) {
    close_descriptor();
    descriptor_ = std::exchange(other.descriptor_, -1);
    directories_ = std::move(other.directories_);
  }
  return *this;
}

FileWatcher::~FileWatcher() { close_descriptor(); }

void FileWatcher::close_descriptor() {
  if (descriptor_ >= 0) {
    static_cast<void>(::close(descriptor_));
    descriptor_ = -1;
  }
}

std::expected<FileWatcher, std::string>
FileWatcher::create(const std::vector<std::filesystem::path> &paths) {
  const int descriptor = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (descriptor < 0) {
    return std::unexpected(system_error("inotify_init1"));
  }
  FileWatcher watcher{descriptor};
  if (auto replaced = watcher.replace_paths(paths); !replaced) {
    return std::unexpected(std::move(replaced.error()));
  }
  return watcher;
}

std::expected<void, std::string>
FileWatcher::replace_paths(const std::vector<std::filesystem::path> &paths) {
  std::map<std::filesystem::path, std::set<std::string, std::less<>>> desired;
  for (const auto &path : paths) {
    const auto normalized = path.lexically_normal();
    if (normalized.filename().empty() || normalized.parent_path().empty()) {
      return std::unexpected("watched paths require a parent directory and filename");
    }
    desired[normalized.parent_path()].insert(normalized.filename().string());
  }

  std::map<std::filesystem::path, int> existing;
  for (const auto &[descriptor, directory] : directories_) {
    existing.emplace(directory.path, descriptor);
  }

  std::map<int, DirectoryWatch> replacement;
  std::vector<int> added;
  for (auto &[directory, filenames] : desired) {
    int watch_descriptor = -1;
    if (const auto found = existing.find(directory); found != existing.end()) {
      watch_descriptor = found->second;
    } else {
      watch_descriptor = ::inotify_add_watch(descriptor_, directory.c_str(), watch_mask);
      if (watch_descriptor < 0) {
        const auto error = system_error("inotify_add_watch " + directory.string());
        for (const int added_descriptor : added) {
          static_cast<void>(::inotify_rm_watch(descriptor_, added_descriptor));
        }
        return std::unexpected(error);
      }
      added.push_back(watch_descriptor);
    }
    replacement.emplace(watch_descriptor, DirectoryWatch{directory, std::move(filenames)});
  }

  for (const auto &[watch_descriptor, directory] : directories_) {
    if (!desired.contains(directory.path)) {
      static_cast<void>(::inotify_rm_watch(descriptor_, watch_descriptor));
    }
  }
  directories_ = std::move(replacement);
  return {};
}

std::expected<bool, std::string> FileWatcher::poll() {
  alignas(inotify_event) std::array<std::byte, 64 * 1024> buffer{};
  bool changed = false;
  while (true) {
    const auto count = ::read(descriptor_, buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return changed;
      }
      return std::unexpected(system_error("read inotify events"));
    }
    if (count == 0) {
      return changed;
    }

    std::size_t offset = 0;
    const auto byte_count = static_cast<std::size_t>(count);
    while (offset + sizeof(inotify_event) <= byte_count) {
      const auto *event = reinterpret_cast<const inotify_event *>(buffer.data() + offset);
      const auto event_size = sizeof(inotify_event) + event->len;
      if (offset + event_size > byte_count) {
        return std::unexpected("truncated inotify event");
      }
      if ((event->mask & IN_Q_OVERFLOW) != 0U) {
        return std::unexpected("inotify event queue overflow");
      }
      if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0U) {
        return std::unexpected("watched directory was removed or moved");
      }
      if ((event->mask & IN_IGNORED) == 0U && event->len > 0U) {
        if (const auto directory = directories_.find(event->wd); directory != directories_.end()) {
          const std::string_view filename{event->name, ::strnlen(event->name, event->len)};
          changed = changed || directory->second.filenames.contains(filename);
        }
      }
      offset += event_size;
    }
    if (offset != byte_count) {
      return std::unexpected("truncated inotify event buffer");
    }
  }
}

} // namespace gisland
