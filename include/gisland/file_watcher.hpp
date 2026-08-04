#pragma once

#include <chrono>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace gisland {

class ReloadDebouncer {
public:
  explicit ReloadDebouncer(std::chrono::steady_clock::duration quiet_period);

  void observe(std::chrono::steady_clock::time_point now);
  [[nodiscard]] bool consume_due(std::chrono::steady_clock::time_point now);
  void clear();
  [[nodiscard]] bool pending() const;

private:
  std::chrono::steady_clock::duration quiet_period_;
  std::optional<std::chrono::steady_clock::time_point> deadline_;
};

class FileWatcher {
public:
  FileWatcher(const FileWatcher &) = delete;
  FileWatcher &operator=(const FileWatcher &) = delete;
  FileWatcher(FileWatcher &&other) noexcept;
  FileWatcher &operator=(FileWatcher &&other) noexcept;
  ~FileWatcher();

  [[nodiscard]] static std::expected<FileWatcher, std::string>
  create(const std::vector<std::filesystem::path> &paths);
  [[nodiscard]] std::expected<void, std::string>
  replace_paths(const std::vector<std::filesystem::path> &paths);
  [[nodiscard]] std::expected<bool, std::string> poll();

private:
  struct DirectoryWatch {
    std::filesystem::path path;
    std::set<std::string, std::less<>> filenames;
  };

  explicit FileWatcher(int descriptor);
  void close_descriptor();

  int descriptor_{-1};
  std::map<int, DirectoryWatch> directories_;
};

} // namespace gisland
