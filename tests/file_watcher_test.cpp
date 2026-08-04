#include "gisland/file_watcher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() / ("gisland-watcher-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }
  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path &path, std::string_view contents) {
  std::ofstream stream{path};
  if (!stream) {
    throw std::runtime_error{"unable to create watcher fixture"};
  }
  stream << contents;
}

} // namespace

TEST_CASE("reload debounce extends its quiet period and consumes once") {
  using namespace std::chrono_literals;
  const auto start = std::chrono::steady_clock::time_point{};
  gisland::ReloadDebouncer debounce{100ms};

  CHECK_FALSE(debounce.pending());
  debounce.observe(start);
  CHECK_FALSE(debounce.consume_due(start + 99ms));
  debounce.observe(start + 50ms);
  CHECK_FALSE(debounce.consume_due(start + 149ms));
  CHECK(debounce.consume_due(start + 150ms));
  CHECK_FALSE(debounce.consume_due(start + 200ms));

  debounce.observe(start + 300ms);
  debounce.clear();
  CHECK_FALSE(debounce.pending());
  CHECK_FALSE(debounce.consume_due(start + 500ms));
}

TEST_CASE("file watcher filters unrelated files and detects writes and atomic replacement") {
  TemporaryDirectory temporary;
  const auto target = temporary.path() / "config.toml";
  write_file(target, "first");
  auto watcher = gisland::FileWatcher::create({target});
  REQUIRE(watcher.has_value());

  write_file(temporary.path() / "config.toml.tmp", "ignored");
  auto unrelated = watcher->poll();
  REQUIRE(unrelated.has_value());
  CHECK_FALSE(*unrelated);

  write_file(target, "second");
  auto written = watcher->poll();
  REQUIRE(written.has_value());
  CHECK(*written);

  const auto replacement = temporary.path() / "replacement";
  write_file(replacement, "third");
  std::filesystem::rename(replacement, target);
  auto replaced = watcher->poll();
  REQUIRE(replaced.has_value());
  CHECK(*replaced);
}

TEST_CASE("file watcher deduplicates directories and atomically replaces targets") {
  TemporaryDirectory temporary;
  const auto first = temporary.path() / "config.toml";
  const auto second = temporary.path() / "theme.toml";
  write_file(first, "first");
  write_file(second, "second");
  auto watcher = gisland::FileWatcher::create({first, first});
  REQUIRE(watcher.has_value());

  auto replaced = watcher->replace_paths({second});
  REQUIRE(replaced.has_value());
  write_file(first, "ignored");
  auto old_target = watcher->poll();
  REQUIRE(old_target.has_value());
  CHECK_FALSE(*old_target);

  write_file(second, "changed");
  auto new_target = watcher->poll();
  REQUIRE(new_target.has_value());
  CHECK(*new_target);
}
