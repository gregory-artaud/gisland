#pragma once

#include "gisland/protocol.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

namespace gisland::test {

class AudioProcess final {
public:
  explicit AudioProcess(const nlohmann::json &state) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() / ("gisland-audio-" + std::to_string(suffix));
    const auto bin = root_ / "bin";
    std::filesystem::create_directories(bin);
    std::filesystem::create_symlink(GISLAND_AUDIO_FAKE_PACTL_PATH, bin / "pactl");
    std::filesystem::create_symlink(GISLAND_AUDIO_FAKE_GISLANDCTL_PATH, bin / "gislandctl");
    const auto host = bin / "gisland-lua-host";
    std::filesystem::copy_file(GISLAND_LUA_HOST_PATH, host);
    state_path_ = root_ / "state.json";
    command_log_ = root_ / "commands.jsonl";
    write(state_path_, state.dump());

    std::array<int, 2> input{};
    std::array<int, 2> output{};
    REQUIRE(::pipe(input.data()) == 0);
    REQUIRE(::pipe(output.data()) == 0);
    pid_ = ::fork();
    REQUIRE(pid_ >= 0);
    if (pid_ == 0) {
      static_cast<void>(::dup2(input[0], STDIN_FILENO));
      static_cast<void>(::dup2(output[1], STDOUT_FILENO));
      static_cast<void>(::close(input[0]));
      static_cast<void>(::close(input[1]));
      static_cast<void>(::close(output[0]));
      static_cast<void>(::close(output[1]));
      const std::string path = bin.string() + ":/usr/bin:/bin";
      ::setenv("PATH", path.c_str(), 1);
      ::setenv("HOME", root_.c_str(), 1);
      ::setenv("GISLAND_AUDIO_FAKE_STATE", state_path_.c_str(), 1);
      ::setenv("GISLAND_AUDIO_COMMAND_LOG", command_log_.c_str(), 1);
      ::execl(host.c_str(), host.c_str(), GISLAND_AUDIO_LUA_PATH, nullptr);
      _exit(127);
    }
    static_cast<void>(::close(input[0]));
    static_cast<void>(::close(output[1]));
    input_ = input[1];
    output_ = output[0];

    send({{"type", "init"},
          {"protocol",
           {{"minimum", {{"major", 1}, {"minor", 8}}}, {"maximum", {{"major", 1}, {"minor", 8}}}}},
          {"instance_id", "audio-visual"},
          {"capabilities",
           {"independent-views", "compact-view-styles", "icon-roles", "progress-transitions"}},
          {"configuration", nlohmann::json::object()},
          {"locale", "C"},
          {"timezone", "UTC"}});
    const auto ready = receive();
    REQUIRE(ready.at("type") == "ready");
    REQUIRE(ready.at("protocol_minor") == 8);
  }

  AudioProcess(const AudioProcess &) = delete;
  AudioProcess &operator=(const AudioProcess &) = delete;

  ~AudioProcess() {
    if (input_ >= 0) {
      static_cast<void>(::close(input_));
    }
    if (output_ >= 0) {
      static_cast<void>(::close(output_));
    }
    if (pid_ > 0) {
      ::kill(pid_, SIGTERM);
      static_cast<void>(::waitpid(pid_, nullptr, 0));
    }
    std::filesystem::remove_all(root_);
  }

  [[nodiscard]] PublishMessage action(std::string_view action_id) {
    send({{"type", "action"}, {"action_id", action_id}, {"invocation_id", "1"}});
    std::optional<PublishMessage> publication;
    while (true) {
      const auto record = receive();
      const auto parsed = parse_module_message(record.dump());
      REQUIRE(parsed.has_value());
      if (const auto *publish = std::get_if<PublishMessage>(&*parsed)) {
        publication = *publish;
      }
      if (const auto *result = std::get_if<ActionResultMessage>(&*parsed)) {
        REQUIRE(result->invocation_id == 1);
        REQUIRE(result->accepted);
        REQUIRE(publication.has_value());
        return std::move(*publication);
      }
    }
  }

private:
  static void write(const std::filesystem::path &path, std::string_view content) {
    std::ofstream stream{path};
    if (!stream) {
      throw std::runtime_error{"could not write audio fixture"};
    }
    stream << content;
  }

  void send(const nlohmann::json &record) const {
    const std::string line = record.dump() + '\n';
    std::size_t offset = 0;
    while (offset < line.size()) {
      const auto written = ::write(input_, line.data() + offset, line.size() - offset);
      REQUIRE(written > 0);
      offset += static_cast<std::size_t>(written);
    }
  }

  [[nodiscard]] nlohmann::json receive() {
    while (true) {
      const auto newline = buffered_.find('\n');
      if (newline != std::string::npos) {
        const auto line = buffered_.substr(0, newline);
        buffered_.erase(0, newline + 1);
        return nlohmann::json::parse(line);
      }
      pollfd descriptor{.fd = output_, .events = POLLIN, .revents = 0};
      REQUIRE(::poll(&descriptor, 1, 3000) == 1);
      std::array<char, 4096> bytes{};
      const auto count = ::read(output_, bytes.data(), bytes.size());
      REQUIRE(count > 0);
      buffered_.append(bytes.data(), static_cast<std::size_t>(count));
    }
  }

  std::filesystem::path root_;
  std::filesystem::path state_path_;
  std::filesystem::path command_log_;
  pid_t pid_{-1};
  int input_{-1};
  int output_{-1};
  std::string buffered_;
};

} // namespace gisland::test
