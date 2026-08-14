#include "gisland/content_fingerprint.hpp"
#include "gisland/glib_main_context.hpp"
#include "gisland/lua_host.hpp"
#include "gisland/lua_transport.hpp"
#include "gisland/poll.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <glib-unix.h>
#include <glib.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef GISLAND_LUA_FIXTURE_DIR
#error "GISLAND_LUA_FIXTURE_DIR must be defined"
#endif

namespace {

class Pipe {
public:
  Pipe() {
    std::array<int, 2> descriptors{};
    REQUIRE(::pipe2(descriptors.data(), O_CLOEXEC | O_NONBLOCK) == 0);
    read_ = descriptors[0];
    write_ = descriptors[1];
  }

  Pipe(const Pipe &) = delete;
  Pipe &operator=(const Pipe &) = delete;

  ~Pipe() {
    if (read_ >= 0) {
      ::close(read_);
    }
    if (write_ >= 0) {
      ::close(write_);
    }
  }

  [[nodiscard]] int read_fd() const { return read_; }
  [[nodiscard]] int write_fd() const { return write_; }

  void close_write() {
    REQUIRE(::close(write_) == 0);
    write_ = -1;
  }

  void close_read() {
    REQUIRE(::close(read_) == 0);
    read_ = -1;
  }

private:
  int read_{-1};
  int write_{-1};
};

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() / ("gisland-lua-host-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path &path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream{path};
  if (!stream) {
    throw std::runtime_error{"could not create Lua host fixture"};
  }
  stream << content;
}

void write_all(int descriptor, std::string_view value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto written = ::write(descriptor, value.data() + offset, value.size() - offset);
    REQUIRE(written > 0);
    offset += static_cast<std::size_t>(written);
  }
}

void configure_lgi_test_environment() {
#ifdef GISLAND_TEST_LGI_PREFIX
  const std::string prefix = GISLAND_TEST_LGI_PREFIX;
  const auto lua_path = prefix + "/share/lua/5.4/?.lua;" + prefix + "/share/lua/5.4/?/init.lua;;";
  const auto lua_cpath = prefix + "/lib/lua/5.4/?.so;;";
  if (::setenv("LUA_PATH", lua_path.c_str(), 1) != 0 ||
      ::setenv("LUA_CPATH", lua_cpath.c_str(), 1) != 0) {
    _exit(126);
  }
#endif
}

[[nodiscard]] std::string read_available(int descriptor) {
  std::string result;
  std::array<char, 4096> buffer{};
  while (true) {
    const auto read = ::read(descriptor, buffer.data(), buffer.size());
    if (read > 0) {
      result.append(buffer.data(), static_cast<std::size_t>(read));
      continue;
    }
    const bool exhausted = read == 0 || (read == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
    REQUIRE(exhausted);
    return result;
  }
}

using Records = std::vector<nlohmann::json>;

volatile sig_atomic_t signal_count = 0;

void count_signal(int) {
  const sig_atomic_t current = signal_count;
  signal_count = current + 1;
}

class ScopedSignalHandler {
public:
  explicit ScopedSignalHandler(int signal) : signal_(signal) {
    struct sigaction action{};
    action.sa_handler = count_signal;
    sigemptyset(&action.sa_mask);
    REQUIRE(::sigaction(signal_, &action, &previous_) == 0);
  }

  ~ScopedSignalHandler() { static_cast<void>(::sigaction(signal_, &previous_, nullptr)); }

private:
  int signal_;
  struct sigaction previous_{};
};

[[nodiscard]] gisland::LuaHost::Emit collect_into(Records &records) {
  return [&records](nlohmann::json record) -> std::expected<void, gisland::LuaHostError> {
    records.push_back(std::move(record));
    return {};
  };
}

[[nodiscard]] nlohmann::json init_record(nlohmann::json configuration = {{"answer", 42}}) {
  return {
      {"type", "init"},
      {"protocol",
       {{"minimum", {{"major", 1}, {"minor", 0}}}, {"maximum", {{"major", 1}, {"minor", 8}}}}},
      {"instance_id", "test"},
      {"capabilities", {"data-snapshots"}},
      {"configuration", std::move(configuration)},
      {"locale", "en_US.UTF-8"},
      {"timezone", "UTC"},
  };
}

[[nodiscard]] std::size_t open_descriptor_count() {
  return static_cast<std::size_t>(std::distance(
      std::filesystem::directory_iterator{"/proc/self/fd"}, std::filesystem::directory_iterator{}));
}

[[nodiscard]] std::expected<std::unique_ptr<gisland::LuaTransport>, gisland::LuaTransportError>
make_transport(Pipe &input, Pipe &output, Records &records) {
  return gisland::LuaTransport::create(
      input.read_fd(), output.write_fd(),
      [&records](const nlohmann::json &record, const gisland::LuaTransport::Emit &emit) {
        records.push_back(record);
        return emit(record);
      });
}

} // namespace

TEST_CASE("lua host transport frames partial and multiple JSONL records") {
  Pipe input;
  Pipe output;
  Records records;
  auto transport = make_transport(input, output, records);
  REQUIRE(transport.has_value());

  write_all(input.write_fd(), R"({"type":"one")");
  REQUIRE((*transport)->poll_once(0).has_value());
  CHECK(records.empty());

  write_all(input.write_fd(), "}\n{\"type\":\"two\"}\n");
  REQUIRE((*transport)->poll_once(0).has_value());
  REQUIRE(records.size() == 1);
  REQUIRE((*transport)->poll_once(0).has_value());
  REQUIRE((*transport)->poll_once(0).has_value());
  REQUIRE(records.size() == 2);
  CHECK(records[0].at("type") == "one");
  CHECK(records[1].at("type") == "two");

  REQUIRE((*transport)->poll_once(0).has_value());
  const auto output_text = read_available(output.read_fd());
  CHECK(output_text == "{\"type\":\"one\"}\n{\"type\":\"two\"}\n");
}

TEST_CASE("lua host transport rejects EOF malformed records NUL and non-objects") {
  Pipe input;
  Pipe output;
  Records records;
  auto transport = make_transport(input, output, records);
  REQUIRE(transport.has_value());

  SECTION("EOF") {
    input.close_write();
    const auto result = (*transport)->poll_once(0);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaTransportErrorCode::input_eof);
  }

  SECTION("malformed JSON") {
    write_all(input.write_fd(), "{broken}\n");
    const auto result = (*transport)->poll_once(0);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaTransportErrorCode::malformed_json);
  }

  SECTION("non-object JSON") {
    write_all(input.write_fd(), "[]\n");
    const auto result = (*transport)->poll_once(0);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaTransportErrorCode::non_object);
  }

  SECTION("embedded NUL") {
    const std::string record{"{\"type\":\"x\"}\0\n", 14};
    write_all(input.write_fd(), record);
    const auto result = (*transport)->poll_once(0);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaTransportErrorCode::embedded_nul);
  }
}

TEST_CASE("lua host transport bounds input and output records") {
  Pipe input;
  Pipe output;
  Records records;
  auto transport = make_transport(input, output, records);
  REQUIRE(transport.has_value());

  SECTION("input") {
    const std::string chunk(64 * 1024, 'x');
    for (std::size_t written = 0; written <= gisland::LuaTransport::max_record_bytes;
         written += chunk.size()) {
      write_all(input.write_fd(), chunk);
      const auto result = (*transport)->poll_once(0);
      if (!result) {
        CHECK(result.error().code == gisland::LuaTransportErrorCode::record_too_large);
        return;
      }
    }
    const auto result = (*transport)->poll_once(0);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaTransportErrorCode::record_too_large);
  }

  SECTION("output") {
    nlohmann::json oversized = {
        {"value", std::string(gisland::LuaTransport::max_record_bytes, 'x')}};
    const auto result = (*transport)->send(std::move(oversized));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaTransportErrorCode::record_too_large);
  }
}

TEST_CASE("lua host transport preserves queued output across partial writes") {
  Pipe input;
  Pipe output;
  Records records;
  auto transport = make_transport(input, output, records);
  REQUIRE(transport.has_value());

  const nlohmann::json record = {{"value", std::string(256 * 1024, 'x')}};
  REQUIRE((*transport)->send(record).has_value());
  const auto queued = (*transport)->pending_output_bytes();
  REQUIRE(queued > 0);

  REQUIRE((*transport)->poll_once(0).has_value());
  CHECK((*transport)->pending_output_bytes() > 0);
  CHECK((*transport)->pending_output_bytes() < queued);

  std::string output_text;
  while ((*transport)->pending_output_bytes() > 0) {
    output_text += read_available(output.read_fd());
    REQUIRE((*transport)->poll_once(0).has_value());
  }
  output_text += read_available(output.read_fd());
  CHECK(nlohmann::json::parse(output_text) == record);
}

TEST_CASE("lua host transport leaves input unread while output is pending") {
  Pipe input;
  Pipe output;
  Records records;
  auto transport = make_transport(input, output, records);
  REQUIRE(transport.has_value());

  REQUIRE((*transport)->send({{"pending", true}}).has_value());
  write_all(input.write_fd(), "{\"sequence\":1}\n{\"sequence\":2}\n");

  auto descriptors = (*transport)->poll_descriptors(false);
  REQUIRE(gisland::poll_with_timeout(descriptors, 0).has_value());
  REQUIRE((*transport)->advance(descriptors).has_value());
  CHECK(records.empty());

  while ((*transport)->pending_output_bytes() > 0) {
    static_cast<void>(read_available(output.read_fd()));
    descriptors = (*transport)->poll_descriptors(false);
    REQUIRE(gisland::poll_with_timeout(descriptors, 0).has_value());
    REQUIRE((*transport)->advance(descriptors).has_value());
  }

  descriptors = (*transport)->poll_descriptors(true);
  REQUIRE(gisland::poll_with_timeout(descriptors, 0).has_value());
  REQUIRE((*transport)->advance(descriptors).has_value());
  REQUIRE(records.size() == 1);
  CHECK(records.front().at("sequence") == 1);
}

TEST_CASE("lua host transport detects output failure while input is disabled") {
  Pipe input;
  Pipe output;
  Records records;
  auto transport = make_transport(input, output, records);
  REQUIRE(transport.has_value());
  REQUIRE((*transport)->send({{"pending", true}}).has_value());
  output.close_read();

  auto descriptors = (*transport)->poll_descriptors(false);
  REQUIRE(gisland::poll_with_timeout(descriptors, 0).has_value());
  const auto advanced = (*transport)->advance(descriptors);
  REQUIRE_FALSE(advanced.has_value());
  CHECK(advanced.error().code == gisland::LuaTransportErrorCode::write_failed);
}

TEST_CASE("lua host transport fails rather than dropping output on queue overflow") {
  Pipe input;
  Pipe output;
  Records records;
  auto transport = make_transport(input, output, records);
  REQUIRE(transport.has_value());

  for (std::size_t index = 0; index < gisland::LuaTransport::max_output_messages; ++index) {
    REQUIRE((*transport)->send({{"index", index}}).has_value());
  }
  const auto result = (*transport)->send({{"overflow", true}});
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == gisland::LuaTransportErrorCode::queue_overflow);
  CHECK((*transport)->pending_output_messages() == gisland::LuaTransport::max_output_messages);
}

TEST_CASE("lua host transport enforces the output queue byte limit") {
  Pipe input;
  Pipe output;
  Records records;
  auto transport = make_transport(input, output, records);
  REQUIRE(transport.has_value());

  const nlohmann::json large = {{"value", std::string(6 * 1024 * 1024, 'x')}};
  REQUIRE((*transport)->send(large).has_value());
  REQUIRE((*transport)->send(large).has_value());
  const auto result = (*transport)->send(large);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == gisland::LuaTransportErrorCode::queue_overflow);
  CHECK((*transport)->pending_output_messages() == 2);
  CHECK((*transport)->pending_output_bytes() < gisland::LuaTransport::max_output_bytes);
  CHECK(gisland::LuaTransport::max_output_bytes == std::size_t{16} * 1024U * 1024U);
}

TEST_CASE("lua host transport does not open or own injected descriptors") {
  const auto before = open_descriptor_count();
  {
    Pipe input;
    Pipe output;
    const auto with_pipes = open_descriptor_count();
    Records records;
    auto transport = make_transport(input, output, records);
    REQUIRE(transport.has_value());
    CHECK(open_descriptor_count() == with_pipes);
  }
  CHECK(open_descriptor_count() == before);
}

TEST_CASE("GLib main context adapter supports cancel reprepare check and destruction") {
  Pipe wakeup;
  int dispatches = 0;
  GSource *source = g_unix_fd_source_new(wakeup.read_fd(), G_IO_IN);
  g_source_set_callback(source, G_SOURCE_FUNC(+[](gint, GIOCondition, gpointer data) -> gboolean {
                          ++*static_cast<int *>(data);
                          return G_SOURCE_REMOVE;
                        }),
                        &dispatches, nullptr);
  g_source_attach(source, nullptr);

  const auto before = open_descriptor_count();
  {
    gisland::GlibMainContext context;
    auto query = context.prepare();
    REQUIRE(query.has_value());
    context.cancel_poll();

    query = context.prepare();
    REQUIRE(query.has_value());
    const auto descriptor =
        std::ranges::find_if(query->descriptors, [&wakeup](const pollfd &candidate) {
          return candidate.fd == wakeup.read_fd();
        });
    REQUIRE(descriptor != query->descriptors.end());
    CHECK((descriptor->events & POLLIN) != 0);

    write_all(wakeup.write_fd(), "x");
    REQUIRE(gisland::poll_with_timeout(query->descriptors, 0).has_value());
    CHECK((descriptor->revents & POLLIN) != 0);
    REQUIRE(context.check_and_dispatch(query->descriptors).has_value());
    CHECK(dispatches == 1);
  }
  CHECK(open_descriptor_count() == before);
  g_source_unref(source);
}

TEST_CASE("lua host script reports entry loading failures", "[lua_host_script]") {
  const auto fixtures = std::filesystem::path{GISLAND_LUA_FIXTURE_DIR};

  SECTION("missing file") {
    const auto host = gisland::LuaHost::load((fixtures / "missing.lua").string());
    REQUIRE_FALSE(host.has_value());
    CHECK(host.error().code == gisland::LuaHostErrorCode::file_error);
  }
  SECTION("syntax error") {
    const auto host = gisland::LuaHost::load((fixtures / "syntax_error.lua").string());
    REQUIRE_FALSE(host.has_value());
    CHECK(host.error().code == gisland::LuaHostErrorCode::syntax_error);
  }
  SECTION("top-level runtime error") {
    const auto host = gisland::LuaHost::load((fixtures / "runtime_error.lua").string());
    REQUIRE_FALSE(host.has_value());
    CHECK(host.error().code == gisland::LuaHostErrorCode::runtime_error);
    CHECK(host.error().message.find("top-level failure") != std::string::npos);
  }
}

TEST_CASE("lua host script requires exactly the registered returned definition",
          "[lua_host_script]") {
  const auto fixtures = std::filesystem::path{GISLAND_LUA_FIXTURE_DIR};

  const auto check_error = [&](std::string_view fixture, gisland::LuaHostErrorCode code) {
    const auto host = gisland::LuaHost::load((fixtures / fixture).string());
    REQUIRE_FALSE(host.has_value());
    CHECK(host.error().code == code);
  };

  SECTION("nil") { check_error("nil.lua", gisland::LuaHostErrorCode::missing_definition); }
  SECTION("no return") {
    check_error("no_return.lua", gisland::LuaHostErrorCode::missing_definition);
  }
  SECTION("raw table") {
    check_error("raw_table.lua", gisland::LuaHostErrorCode::missing_definition);
  }
  SECTION("duplicate call") {
    check_error("duplicate.lua", gisland::LuaHostErrorCode::invalid_definition);
  }
  SECTION("different returned table") {
    check_error("wrong_return.lua", gisland::LuaHostErrorCode::invalid_return);
  }
}

TEST_CASE("lua host script validates module fields and actions", "[lua_host_script]") {
  const auto fixture = std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "generated-invalid.lua";
  const auto check_script = [&](std::string_view body, std::string_view diagnostic) {
    {
      std::ofstream output{fixture};
      REQUIRE(output.good());
      output << body;
    }
    const auto host = gisland::LuaHost::load(fixture.string());
    std::filesystem::remove(fixture);
    REQUIRE_FALSE(host.has_value());
    CHECK(host.error().code == gisland::LuaHostErrorCode::invalid_definition);
    CHECK(host.error().message.find(diagnostic) != std::string::npos);
  };

  SECTION("unknown field") {
    check_script("return gisland.module { surprise = true }", "unknown module field 'surprise'");
  }
  SECTION("invalid every") {
    check_script("return gisland.module { every = 1 }", "field 'every' must be a string");
  }
  SECTION("invalid callback") {
    check_script("return gisland.module { update = true }", "field 'update' must be a function");
  }
  SECTION("invalid actions table") {
    check_script("return gisland.module { actions = true }", "field 'actions' must be a table");
  }
  SECTION("invalid action key") {
    check_script("return gisland.module { actions = { [1] = function() end } }",
                 "action names must be strings");
  }
  SECTION("invalid action callback") {
    check_script("return gisland.module { actions = { refresh = true } }",
                 "action 'refresh' must be a function");
  }
}

TEST_CASE("lua host script retains callbacks without stack debris", "[lua_host_script]") {
  const auto entry = std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "valid.lua";
  auto host = gisland::LuaHost::load(entry.string());
  REQUIRE(host.has_value());

  const auto &definition = (*host)->definition();
  CHECK(definition.every == "1s");
  CHECK(definition.has_init);
  CHECK(definition.has_update);
  CHECK(definition.has_visibility);
  CHECK(definition.has_shutdown);
  CHECK(definition.actions == std::vector<std::string>{"refresh", "set-value"});
  CHECK((*host)->retained_callback_count() == 6);
  CHECK((*host)->stack_size() == 0);
}

TEST_CASE("lua host prepends package require paths and preserves inherited cpath",
          "[lua_host_script][lua_host_require]") {
  TemporaryDirectory temporary;
  const auto package = temporary.path() / "package";
  const auto inherited_first = temporary.path() / "inherited-first";
  const auto inherited_second = temporary.path() / "inherited-second";
  const auto unrelated_cwd = temporary.path() / "cwd";
  std::filesystem::create_directories(unrelated_cwd);
  write_file(package / "choice.lua", "return 'package'\n");
  write_file(package / "nested/init.lua", "return 'nested-package'\n");
  write_file(inherited_first / "choice.lua", "return 'inherited-first'\n");
  write_file(inherited_second / "choice.lua", "return 'inherited-second'\n");

  const auto inherited_path =
      (inherited_first / "?.lua").string() + ";" + (inherited_second / "?.lua").string();
  const std::string inherited_cpath = "/native/first/?.so;/native/second/?.so";
  const auto expected_path =
      (package / "?.lua").string() + ";" + (package / "?/init.lua").string() + ";" + inherited_path;
  const auto entry = package / "entry.lua";
  write_file(entry, "assert(package.path == " + nlohmann::json(expected_path).dump() + ")\n" +
                        "assert(package.cpath == " + nlohmann::json(inherited_cpath).dump() +
                        ")\n"
                        "assert(require('choice') == 'package')\n"
                        "assert(require('nested') == 'nested-package')\n"
                        "return gisland.module {}\n");

  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    if (::chdir(unrelated_cwd.c_str()) != 0 ||
        ::setenv("LUA_PATH", inherited_path.c_str(), 1) != 0 ||
        ::setenv("LUA_CPATH", inherited_cpath.c_str(), 1) != 0) {
      _exit(126);
    }
    const auto host = gisland::LuaHost::load(entry.string());
    _exit(host.has_value() ? 0 : 1);
  }

  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("lua_host_lifecycle initializes before ready with protocol 1.8", "[lua_host_lifecycle]") {
  auto host = gisland::LuaHost::load(
      (std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "lifecycle.lua").string());
  REQUIRE(host.has_value());
  Records records;

  const auto result = (*host)->handle(init_record(), collect_into(records), {});

  REQUIRE(result == gisland::LuaHostState::running);
  REQUIRE(records.size() == 1);
  CHECK(records[0] == nlohmann::json{{"type", "ready"},
                                     {"protocol_major", 1},
                                     {"protocol_minor", 8},
                                     {"capabilities", {"data-snapshots"}}});
  CHECK((*host)->initialized());
}

TEST_CASE("lua_host_lifecycle rejects invalid records", "[lua_host_lifecycle]") {
  const auto entry = std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "lifecycle.lua";

  SECTION("init must be first") {
    auto host = gisland::LuaHost::load(entry.string());
    REQUIRE(host.has_value());
    Records records;
    const auto result = (*host)->handle({{"type", "visibility"}, {"visibility", "hidden"}},
                                        collect_into(records), {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaHostErrorCode::protocol_error);
  }
  SECTION("duplicate init") {
    auto host = gisland::LuaHost::load(entry.string());
    REQUIRE(host.has_value());
    Records records;
    REQUIRE((*host)->handle(init_record(), collect_into(records), {}).has_value());
    const auto result = (*host)->handle(init_record(), collect_into(records), {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaHostErrorCode::protocol_error);
  }
  SECTION("maximum must offer protocol 1.8") {
    auto host = gisland::LuaHost::load(entry.string());
    REQUIRE(host.has_value());
    Records records;
    auto record = init_record();
    record["protocol"]["maximum"]["minor"] = 7;
    const auto result = (*host)->handle(record, collect_into(records), {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaHostErrorCode::protocol_error);
  }
  SECTION("capabilities used by later emissions are not required at init") {
    auto host = gisland::LuaHost::load(entry.string());
    REQUIRE(host.has_value());
    Records records;
    auto record = init_record();
    record["capabilities"] = nlohmann::json::array();
    const auto result = (*host)->handle(record, collect_into(records), {});
    REQUIRE(result.has_value());
    REQUIRE(records.size() == 1);
    CHECK(records[0].at("capabilities") == nlohmann::json::array());
  }
  SECTION("unknown fields are rejected") {
    auto host = gisland::LuaHost::load(entry.string());
    REQUIRE(host.has_value());
    Records records;
    auto record = init_record();
    record["extra"] = true;
    const auto result = (*host)->handle(record, collect_into(records), {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaHostErrorCode::protocol_error);
  }
}

TEST_CASE("lua_host_lifecycle advertises the implemented offered scene capabilities",
          "[lua_host_lifecycle]") {
  auto host = gisland::LuaHost::load(
      (std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "lifecycle.lua").string());
  REQUIRE(host.has_value());
  Records records;
  auto record = init_record();
  record["capabilities"] = {
      "progress-transitions", "not-implemented",  "data-snapshots", "icon-roles",
      "compact-view-styles",  "status-indicator", "ring-progress",  "independent-views",
      "rich-content",         "context-images",
  };

  REQUIRE((*host)->handle(record, collect_into(records), {}).has_value());

  REQUIRE(records.size() == 1);
  CHECK(records[0].at("capabilities") ==
        nlohmann::json{"data-snapshots", "context-images", "rich-content", "independent-views",
                       "ring-progress", "status-indicator", "compact-view-styles", "icon-roles",
                       "progress-transitions"});
}

TEST_CASE("lua_host_lifecycle passes core locale and timezone to init metadata",
          "[lua_host_lifecycle]") {
  TemporaryDirectory temporary;
  const auto entry = temporary.path() / "metadata.lua";
  write_file(entry, R"lua(return gisland.module {
  init = function(configuration, metadata)
    gisland.data {
      answer = configuration.answer,
      instance_id = metadata.instance_id,
      locale = metadata.locale,
      timezone = metadata.timezone,
    }
  end,
})lua");
  auto host = gisland::LuaHost::load(entry.string());
  REQUIRE(host.has_value());
  Records records;

  REQUIRE((*host)->handle(init_record(), collect_into(records), {}).has_value());

  REQUIRE(records.size() == 2);
  CHECK(records[0] == nlohmann::json{{"type", "ready"},
                                     {"protocol_major", 1},
                                     {"protocol_minor", 8},
                                     {"capabilities", {"data-snapshots"}}});
  CHECK(records[1] == nlohmann::json{{"type", "data"},
                                     {"value",
                                      {{"answer", 42},
                                       {"instance_id", "test"},
                                       {"locale", "en_US.UTF-8"},
                                       {"timezone", "UTC"}}}});
}

TEST_CASE("lua_host_lifecycle treats init callback failure as fatal", "[lua_host_lifecycle]") {
  auto host = gisland::LuaHost::load(
      (std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "init_error.lua").string());
  REQUIRE(host.has_value());
  Records records;

  const auto result = (*host)->handle(init_record(), collect_into(records), {});

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == gisland::LuaHostErrorCode::callback_error);
  CHECK(result.error().message.find("init failed") != std::string::npos);
  CHECK(records.empty());
}

TEST_CASE("lua host bounds output buffered during init by message count", "[lua_host_lifecycle]") {
  const auto fixture = std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "generated-init-count.lua";
  {
    std::ofstream output{fixture};
    REQUIRE(output.good());
    output << "return gisland.module { init = function() for i = 1, 257 do "
              "gisland.data { index = i } end end }";
  }
  auto host = gisland::LuaHost::load(fixture.string());
  std::filesystem::remove(fixture);
  REQUIRE(host.has_value());
  Records records;

  const auto result = (*host)->handle(init_record(), collect_into(records), {});

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == gisland::LuaHostErrorCode::callback_error);
  CHECK(result.error().message.find("output queue limit exceeded") != std::string::npos);
  CHECK(records.empty());
}

TEST_CASE("lua host bounds output buffered during init by serialized bytes",
          "[lua_host_lifecycle]") {
  const auto fixture = std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "generated-init-bytes.lua";
  {
    std::ofstream output{fixture};
    REQUIRE(output.good());
    output << "local value = string.rep('x', 6 * 1024 * 1024)\n"
              "return gisland.module { init = function() for i = 1, 3 do "
              "gisland.data { value = value } end end }";
  }
  auto host = gisland::LuaHost::load(fixture.string());
  std::filesystem::remove(fixture);
  REQUIRE(host.has_value());
  Records records;

  const auto result = (*host)->handle(init_record(), collect_into(records), {});

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == gisland::LuaHostErrorCode::callback_error);
  CHECK(result.error().message.find("output queue limit exceeded") != std::string::npos);
  CHECK(records.empty());
}

TEST_CASE("lua host reserves the expanded item budget for explicit data output",
          "[lua_host_lifecycle][lua_host_data]") {
  const auto entry = std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "generated-data-items.lua";
  write_file(entry, R"lua(local function object(size)
  local value = {}
  for index = 1, size do value[tostring(index)] = index end
  return value
end
return gisland.module {
  init = function()
    gisland.data(object(512))
    assert(not pcall(gisland.data, object(513)))
  end,
})lua");
  auto host = gisland::LuaHost::load(entry.string());
  std::filesystem::remove(entry);
  REQUIRE(host.has_value());
  Records records;

  REQUIRE((*host)->handle(init_record(), collect_into(records), {}).has_value());
  REQUIRE(records.size() == 2);
  CHECK(records[1].at("value").size() == 512);
}

TEST_CASE("lua host keeps configuration action and update values at 256 items",
          "[lua_host_lifecycle][lua_host_data]") {
  const auto object = [] {
    nlohmann::json value = nlohmann::json::object();
    for (int index = 1; index <= 257; ++index) {
      value[std::to_string(index)] = index;
    }
    return value;
  }();

  SECTION("configuration") {
    auto host = gisland::LuaHost::load("config-items.lua", R"lua(return gisland.module {
      init = function() error('configuration should be rejected before init') end,
    })lua");
    REQUIRE(host.has_value());
    Records records;
    const auto result = (*host)->handle(init_record(object), collect_into(records), {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaHostErrorCode::value_error);
    CHECK(result.error().message.find("256") != std::string::npos);
  }

  SECTION("action") {
    auto host = gisland::LuaHost::load("action-items.lua", R"lua(return gisland.module {
      actions = { inspect = function() return true end },
    })lua");
    REQUIRE(host.has_value());
    Records records;
    const auto emit = collect_into(records);
    REQUIRE((*host)->handle(init_record(), emit, {}).has_value());
    const auto result = (*host)->handle(
        {{"type", "action"}, {"action_id", "inspect"}, {"value", object}}, emit, {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaHostErrorCode::value_error);
    CHECK(result.error().message.find("256") != std::string::npos);
  }

  SECTION("update return") {
    auto host = gisland::LuaHost::load("update-items.lua", R"lua(local function object(size)
      local value = {}
      for index = 1, size do value[tostring(index)] = index end
      return value
    end
    return gisland.module { every = '1ms', update = function() return object(257) end })lua");
    REQUIRE(host.has_value());
    Records records;
    const auto emit = collect_into(records);
    REQUIRE((*host)->handle(init_record(), emit, {}).has_value());
    const auto result =
        (*host)->run_due(gisland::LuaHost::TimePoint{} + std::chrono::milliseconds{1}, emit);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaHostErrorCode::value_error);
    CHECK(result.error().message.find("256") != std::string::npos);
  }
}

TEST_CASE("lua callbacks report invalid values and timer requests without leaking",
          "[lua_host_lifecycle][lua_host_timer][lua_host_data]") {
  const auto fixture = std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "generated-boundaries.lua";
  {
    std::ofstream output{fixture};
    REQUIRE(output.good());
    output << R"lua(return gisland.module { init = function()
      assert(not pcall(gisland.data, function() end))
      assert(not pcall(gisland.data, { value = function() end }))
      assert(not pcall(gisland.after, "invalid", function() end))
      assert(not pcall(gisland.after, "1ms", true))
      for i = 1, 256 do gisland.defer(function() end) end
      assert(not pcall(gisland.defer, function() end))
      gisland.data { survived = true }
    end })lua";
  }
  auto host = gisland::LuaHost::load(fixture.string());
  std::filesystem::remove(fixture);
  REQUIRE(host.has_value());
  Records records;

  const auto result = (*host)->handle(init_record(), collect_into(records), {});

  REQUIRE(result.has_value());
  REQUIRE(records.size() == 2);
  CHECK(records[1].at("value").at("survived") == true);
}

TEST_CASE("lua_host_lifecycle treats shutdown callback failure as fatal", "[lua_host_lifecycle]") {
  auto host = gisland::LuaHost::load(
      (std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "shutdown_error.lua").string());
  REQUIRE(host.has_value());
  Records records;
  const auto emit = collect_into(records);
  REQUIRE((*host)->handle(init_record(), emit, {}).has_value());

  const auto result =
      (*host)->handle({{"type", "shutdown"}, {"reason", "test"}, {"deadline_ms", 100}}, emit, {});

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == gisland::LuaHostErrorCode::callback_error);
  CHECK(result.error().message.find("shutdown failed") != std::string::npos);
  CHECK((*host)->stopped());
}

TEST_CASE("lua_host_lifecycle treats visibility callback failure as fatal",
          "[lua_host_lifecycle]") {
  auto host = gisland::LuaHost::load(
      (std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "visibility_error.lua").string());
  REQUIRE(host.has_value());
  Records records;
  const auto emit = collect_into(records);
  REQUIRE((*host)->handle(init_record(), emit, {}).has_value());

  const auto result = (*host)->handle({{"type", "visibility"}, {"visibility", "hidden"}}, emit, {});

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == gisland::LuaHostErrorCode::callback_error);
  CHECK(result.error().message.find("visibility failed") != std::string::npos);
}

TEST_CASE("lua_host_lifecycle serializes visibility and shutdown callbacks",
          "[lua_host_lifecycle]") {
  auto host = gisland::LuaHost::load(
      (std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "lifecycle.lua").string());
  REQUIRE(host.has_value());
  Records records;
  const auto emit = collect_into(records);
  REQUIRE((*host)->handle(init_record(), emit, {}).has_value());

  for (const std::string_view visibility : {"hidden", "compact-active", "expanded-active"}) {
    REQUIRE((*host)->handle({{"type", "visibility"}, {"visibility", visibility}}, emit, {}) ==
            gisland::LuaHostState::running);
  }
  const auto shutdown =
      (*host)->handle({{"type", "shutdown"}, {"reason", "test"}, {"deadline_ms", 100}}, emit, {});
  REQUIRE(shutdown == gisland::LuaHostState::stopped);
  CHECK((*host)->stopped());

  const auto duplicate =
      (*host)->handle({{"type", "shutdown"}, {"reason", "again"}, {"deadline_ms", 100}}, emit, {});
  CHECK(duplicate == gisland::LuaHostState::stopped);
}

TEST_CASE("lua_host_lifecycle validates actions before temporarily ignoring them",
          "[lua_host_lifecycle]") {
  auto host = gisland::LuaHost::load(
      (std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "lifecycle.lua").string());
  REQUIRE(host.has_value());
  Records records;
  const auto emit = collect_into(records);
  REQUIRE((*host)->handle(init_record(), emit, {}).has_value());

  CHECK((*host)->handle(
            {{"type", "action"}, {"action_id", "later"}, {"invocation_id", "18446744073709551615"}},
            emit, {}) == gisland::LuaHostState::running);
  const auto invalid = (*host)->handle(
      {{"type", "action"}, {"action_id", "later"}, {"invocation_id", "invalid"}}, emit, {});
  REQUIRE_FALSE(invalid.has_value());
  CHECK(invalid.error().code == gisland::LuaHostErrorCode::protocol_error);
}

TEST_CASE("lua_host_action invokes retained handlers with nil and typed values",
          "[lua_host_action]") {
  auto host = gisland::LuaHost::load(
      (std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "action_module.lua").string());
  REQUIRE(host.has_value());
  Records records;
  const auto emit = collect_into(records);
  REQUIRE((*host)->handle(init_record(), emit, {}).has_value());
  records.clear();

  const std::array<nlohmann::json, 7> values{
      nullptr,
      true,
      42,
      1.5,
      "text",
      nlohmann::json::array({1, "two"}),
      nlohmann::json{{"nested", false}},
  };
  REQUIRE((*host)->handle({{"type", "action"}, {"action_id", "accept"}}, emit, {}).has_value());
  for (const auto &value : values) {
    REQUIRE((*host)
                ->handle({{"type", "action"}, {"action_id", "accept"}, {"value", value}}, emit, {})
                .has_value());
  }

  REQUIRE(records.size() == 16);
  CHECK(records[0] == nlohmann::json{{"type", "data"}, {"value", {{"kind", "nil"}}}});
  CHECK(records[1] ==
        nlohmann::json{{"type", "action_result"}, {"action_id", "accept"}, {"accepted", true}});
  CHECK(records[2] == nlohmann::json{{"type", "data"}, {"value", {{"kind", "nil"}}}});
  for (std::size_t index = 1; index < values.size(); ++index) {
    const auto offset = (index + 1U) * 2U;
    CHECK(records[offset].at("value").at("value") == values[index]);
    CHECK(records[offset + 1U] ==
          nlohmann::json{{"type", "action_result"}, {"action_id", "accept"}, {"accepted", true}});
  }
}

TEST_CASE("lua_host_action returns exact accepted and rejected contracts", "[lua_host_action]") {
  auto host = gisland::LuaHost::load(
      (std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "action_module.lua").string());
  REQUIRE(host.has_value());
  Records records;
  const auto emit = collect_into(records);
  REQUIRE((*host)->handle(init_record(), emit, {}).has_value());
  records.clear();

  REQUIRE((*host)->handle({{"type", "action"}, {"action_id", "reject"}}, emit, {}).has_value());
  REQUIRE(
      (*host)->handle({{"type", "action"}, {"action_id", "reject-reason"}}, emit, {}).has_value());
  REQUIRE((*host)
              ->handle({{"type", "action"},
                        {"action_id", "publish-then-accept"},
                        {"invocation_id", "18446744073709551615"}},
                       emit, {})
              .has_value());

  REQUIRE(records.size() == 5);
  CHECK(records[0] ==
        nlohmann::json{{"type", "action_result"}, {"action_id", "reject"}, {"accepted", false}});
  CHECK(records[1] == nlohmann::json{{"type", "action_result"},
                                     {"action_id", "reject-reason"},
                                     {"accepted", false},
                                     {"message", "not available"}});
  CHECK(records[2] == nlohmann::json{{"type", "data"}, {"value", {{"callback", "published"}}}});
  CHECK(records[3].at("type") == "publish");
  CHECK(records[3].at("context_id") == "action-context");
  CHECK(records[4] == nlohmann::json{{"type", "action_result"},
                                     {"action_id", "publish-then-accept"},
                                     {"accepted", true},
                                     {"invocation_id", "18446744073709551615"}});
}

TEST_CASE("lua_host_action failures log and reject one invocation without stopping",
          "[lua_host_action]") {
  const auto entry = std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "action_module.lua";
  for (const std::string_view action : {"missing", "throws", "invalid-type", "invalid-extra",
                                        "invalid-diagnostic", "long-diagnostic"}) {
    INFO(action);
    auto host = gisland::LuaHost::load(entry.string());
    REQUIRE(host.has_value());
    Records records;
    const auto emit = collect_into(records);
    REQUIRE((*host)->handle(init_record(), emit, {}).has_value());
    records.clear();

    REQUIRE((*host)->handle({{"type", "action"}, {"action_id", action}, {"invocation_id", "42"}},
                            emit, {}) == gisland::LuaHostState::running);

    REQUIRE(records.size() == 2);
    CHECK(records[0].at("type") == "log");
    CHECK(records[0].at("level") == "error");
    CHECK(records[0].at("message").get_ref<const std::string &>().size() <= 4096);
    CHECK(records[1].at("type") == "action_result");
    CHECK(records[1].at("action_id") == action);
    CHECK_FALSE(records[1].at("accepted").get<bool>());
    CHECK(records[1].at("invocation_id") == "42");
    if (action == "missing") {
      CHECK(records[0].at("message") == "unknown action");
      CHECK(records[1].at("message") == "unknown action");
    }
    CHECK((*host)->initialized());
    CHECK_FALSE((*host)->stopped());

    records.clear();
    REQUIRE((*host)->handle({{"type", "action"}, {"action_id", "accept"}}, emit, {}).has_value());
    REQUIRE(records.size() == 2);
    CHECK(records.back().at("accepted") == true);
    CHECK_FALSE(records.back().contains("invocation_id"));
  }
}

TEST_CASE("lua_host_action fallback can reject unknown actions without a diagnostic",
          "[lua_host_action]") {
  TemporaryDirectory temporary;
  const auto entry = temporary.path() / "fallback.lua";
  write_file(entry, R"lua(return gisland.module {
  fallback_action = function(action_id, value)
    if action_id ~= "missing" or value.answer ~= 42 then error("wrong fallback arguments") end
    return false
  end,
})lua");
  auto host = gisland::LuaHost::load(entry.string());
  REQUIRE(host.has_value());
  Records records;
  const auto emit = collect_into(records);
  REQUIRE((*host)->handle(init_record(), emit, {}).has_value());
  records.clear();

  REQUIRE((*host)->handle({{"type", "action"},
                           {"action_id", "missing"},
                           {"value", {{"answer", 42}}},
                           {"invocation_id", "7"}},
                          emit, {}) == gisland::LuaHostState::running);

  REQUIRE(records.size() == 1);
  CHECK(records[0] == nlohmann::json{{"type", "action_result"},
                                     {"action_id", "missing"},
                                     {"accepted", false},
                                     {"invocation_id", "7"}});
}

TEST_CASE("lua_host_action validates the public action envelope", "[lua_host_action]") {
  auto host = gisland::LuaHost::load(
      (std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "action_module.lua").string());
  REQUIRE(host.has_value());
  Records records;
  const auto emit = collect_into(records);
  REQUIRE((*host)->handle(init_record(), emit, {}).has_value());

  for (const auto &record : std::array<nlohmann::json, 6>{
           nlohmann::json{{"type", "action"}},
           nlohmann::json{{"type", "action"}, {"action_id", ""}},
           nlohmann::json{{"type", "action"}, {"action_id", 1}},
           nlohmann::json{{"type", "action"}, {"action_id", std::string(129, 'x')}},
           nlohmann::json{{"type", "action"}, {"action_id", "accept"}, {"invocation_id", ""}},
           nlohmann::json{{"type", "action"}, {"action_id", "accept"}, {"extra", true}},
       }) {
    INFO(record.dump());
    const auto result = (*host)->handle(record, emit, {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaHostErrorCode::protocol_error);
  }
}

TEST_CASE("lua_host_action process echoes correlation and remains ready after callback error",
          "[lua_host_action]") {
  Pipe input;
  Pipe output;
  const auto entry = std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "action_module.lua";
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    static_cast<void>(::dup2(input.read_fd(), STDIN_FILENO));
    static_cast<void>(::dup2(output.write_fd(), STDOUT_FILENO));
    ::execl(GISLAND_LUA_HOST_PATH, GISLAND_LUA_HOST_PATH, entry.c_str(), "--module-option",
            nullptr);
    _exit(127);
  }

  write_all(input.write_fd(), init_record().dump() + "\n");
  write_all(
      input.write_fd(),
      nlohmann::json{{"type", "action"}, {"action_id", "throws"}, {"invocation_id", "7"}}.dump() +
          "\n");
  write_all(input.write_fd(), nlohmann::json{{"type", "action"},
                                             {"action_id", "accept"},
                                             {"value", {{"source", "control"}}},
                                             {"invocation_id", "8"}}
                                      .dump() +
                                  "\n");

  std::string text;
  for (int attempt = 0; attempt < 100 && std::ranges::count(text, '\n') < 5; ++attempt) {
    text += read_available(output.read_fd());
    ::usleep(1000);
  }
  std::vector<nlohmann::json> output_records;
  std::size_t start = 0;
  auto end = text.find('\n', start);
  while (end != std::string::npos) {
    output_records.push_back(nlohmann::json::parse(text.substr(start, end - start)));
    start = end + 1;
    end = text.find('\n', start);
  }
  REQUIRE(output_records.size() == 5);
  CHECK(output_records[0].at("type") == "ready");
  CHECK(output_records[2].at("invocation_id") == "7");
  CHECK_FALSE(output_records[2].at("accepted").get<bool>());
  CHECK(output_records[4].at("invocation_id") == "8");
  CHECK(output_records[4].at("accepted") == true);

  write_all(input.write_fd(),
            nlohmann::json{{"type", "shutdown"}, {"reason", "test"}, {"deadline_ms", 100}}.dump() +
                "\n");
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("lua host process verifies discovered entry bytes before loading") {
  TemporaryDirectory temporary;
  const auto entry = temporary.path() / "entry.lua";
  constexpr std::string_view source = "return gisland.module {}\n";
  write_file(entry, source);
  struct stat metadata{};
  REQUIRE(::stat(entry.c_str(), &metadata) == 0);
  const auto identity = std::to_string(static_cast<std::uint64_t>(metadata.st_dev)) + ':' +
                        std::to_string(static_cast<std::uint64_t>(metadata.st_ino)) + ':' +
                        std::to_string(static_cast<std::uint64_t>(metadata.st_size)) + ':' +
                        std::to_string(metadata.st_mtim.tv_sec) + ':' +
                        std::to_string(metadata.st_mtim.tv_nsec);
  const auto fingerprint = gisland::content_fingerprint(source);
  const auto fingerprint_argument = "--gisland-entry-fingerprint=" + fingerprint;

  SECTION("valid entry starts ready") {
    Pipe input;
    Pipe output;
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
      static_cast<void>(::dup2(input.read_fd(), STDIN_FILENO));
      static_cast<void>(::dup2(output.write_fd(), STDOUT_FILENO));
      static_cast<void>(::setenv("GISLAND_LUA_ENTRY_IDENTITY", identity.c_str(), 1));
      ::execl(GISLAND_LUA_HOST_PATH, GISLAND_LUA_HOST_PATH, entry.c_str(),
              fingerprint_argument.c_str(), "--instance-argument", nullptr);
      _exit(127);
    }
    write_all(input.write_fd(), init_record().dump() + "\n");
    std::string text;
    for (int attempt = 0; attempt < 100 && !text.contains('\n'); ++attempt) {
      text += read_available(output.read_fd());
      ::usleep(1000);
    }
    REQUIRE(text.contains('\n'));
    CHECK(nlohmann::json::parse(text.substr(0, text.find('\n'))).at("type") == "ready");
    write_all(
        input.write_fd(),
        nlohmann::json{{"type", "shutdown"}, {"reason", "test"}, {"deadline_ms", 100}}.dump() +
            "\n");
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
  }

  SECTION("same identity with different bytes is rejected") {
    std::string replacement{source};
    replacement.back() = ' ';
    write_file(entry, replacement);
    const std::array times{metadata.st_atim, metadata.st_mtim};
    REQUIRE(::utimensat(AT_FDCWD, entry.c_str(), times.data(), 0) == 0);

    Pipe errors;
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
      static_cast<void>(::dup2(errors.write_fd(), STDERR_FILENO));
      static_cast<void>(::setenv("GISLAND_LUA_ENTRY_IDENTITY", identity.c_str(), 1));
      ::execl(GISLAND_LUA_HOST_PATH, GISLAND_LUA_HOST_PATH, entry.c_str(),
              fingerprint_argument.c_str(), nullptr);
      _exit(127);
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) != 0);
    CHECK(read_available(errors.read_fd()).contains("module entry content changed before launch"));
  }
}

TEST_CASE("lua host process overrides its executable directory before loading scripts") {
  TemporaryDirectory temporary;
  const auto entry = temporary.path() / "bindir.lua";
  const auto marker = temporary.path() / "bindir.txt";
  const auto expected = std::filesystem::canonical(GISLAND_LUA_HOST_PATH).parent_path().string();
  write_file(entry, "local file = assert(io.open(" + nlohmann::json(marker.string()).dump() +
                        ", 'w'))\n"
                        "file:write(assert(os.getenv('GISLAND_LUA_HOST_BINDIR')))\n"
                        "file:close()\n"
                        "return gisland.module {}\n");

  Pipe input;
  Pipe output;
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    static_cast<void>(::dup2(input.read_fd(), STDIN_FILENO));
    static_cast<void>(::dup2(output.write_fd(), STDOUT_FILENO));
    static_cast<void>(::setenv("GISLAND_LUA_HOST_BINDIR", "/spoofed", 1));
    ::execl(GISLAND_LUA_HOST_PATH, GISLAND_LUA_HOST_PATH, entry.c_str(), nullptr);
    _exit(127);
  }

  write_all(input.write_fd(), init_record().dump() + "\n");
  std::string text;
  for (int attempt = 0; attempt < 100 && !text.contains('\n'); ++attempt) {
    text += read_available(output.read_fd());
    ::usleep(1000);
  }
  REQUIRE(text.contains('\n'));
  CHECK(nlohmann::json::parse(text.substr(0, text.find('\n'))).at("type") == "ready");
  REQUIRE(std::filesystem::exists(marker));
  std::ifstream marker_stream{marker};
  CHECK(std::string{std::istreambuf_iterator<char>{marker_stream}, {}} == expected);

  write_all(input.write_fd(),
            nlohmann::json{{"type", "shutdown"}, {"reason", "test"}, {"deadline_ms", 100}}.dump() +
                "\n");
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("lua_host_timer parses bounded durations", "[lua_host_timer]") {
  using namespace std::chrono_literals;
  CHECK(gisland::LuaHost::parse_duration("1ms") == 1ms);
  CHECK(gisland::LuaHost::parse_duration("2s") == 2s);
  CHECK(gisland::LuaHost::parse_duration("3m") == 3min);
  CHECK(gisland::LuaHost::parse_duration("4h") == 4h);
  for (const std::string_view invalid :
       {"", "0ms", "1", "1.5s", "-1s", "1d", "25h", "999999999999999999999h"}) {
    INFO(invalid);
    CHECK_FALSE(gisland::LuaHost::parse_duration(invalid).has_value());
  }
}

TEST_CASE("lua_host_timer rejects invalid periodic duration during script loading",
          "[lua_host_timer]") {
  const auto fixture = std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "generated-duration.lua";
  {
    std::ofstream output{fixture};
    REQUIRE(output.good());
    output << "return gisland.module { every = '0ms', update = function() end }";
  }
  const auto host = gisland::LuaHost::load(fixture.string());
  std::filesystem::remove(fixture);
  REQUIRE_FALSE(host.has_value());
  CHECK(host.error().code == gisland::LuaHostErrorCode::invalid_definition);
}

TEST_CASE("lua_host_timer and lua_host_data emit deterministically",
          "[lua_host_timer][lua_host_data]") {
  using namespace std::chrono_literals;
  auto host = gisland::LuaHost::load(
      (std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "data_module.lua").string());
  REQUIRE(host.has_value());
  Records records;
  const auto emit = collect_into(records);
  const auto start = gisland::LuaHost::TimePoint{};

  REQUIRE((*host)->handle(init_record(), emit, start).has_value());
  REQUIRE(records.size() == 2);
  CHECK(records[0].at("type") == "ready");
  CHECK(records[1] == nlohmann::json{{"type", "data"}, {"value", {{"sequence", 1}}}});
  CHECK((*host)->next_deadline() == start);

  REQUIRE((*host)->run_due(start, emit).has_value());
  REQUIRE(records.size() == 3);
  CHECK(records.back().at("value").at("sequence") == 2);

  REQUIRE((*host)->run_due(start + 10ms, emit).has_value());
  REQUIRE(records.size() == 5);
  CHECK(records[3].at("value").at("sequence") == 3);
  CHECK(records[4].at("value").at("sequence") == 4);

  REQUIRE((*host)->run_due(start + 20ms, emit).has_value());
  REQUIRE(records.size() == 6);
  CHECK(records.back().at("value").at("sequence") == 5);
  CHECK((*host)->next_deadline() == start + 40ms);
}

TEST_CASE("lua_host_timer defers nested zero-delay callbacks to the next turn",
          "[lua_host_timer]") {
  const auto fixture =
      std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "generated-nested-defer.lua";
  {
    std::ofstream output{fixture};
    REQUIRE(output.good());
    output << R"lua(return gisland.module { init = function()
      gisland.defer(function()
        gisland.data { sequence = 1 }
        gisland.defer(function() gisland.data { sequence = 2 } end)
      end)
    end })lua";
  }
  auto host = gisland::LuaHost::load(fixture.string());
  std::filesystem::remove(fixture);
  REQUIRE(host.has_value());
  Records records;
  const auto emit = collect_into(records);
  const auto start = gisland::LuaHost::TimePoint{};
  REQUIRE((*host)->handle(init_record(), emit, start).has_value());

  REQUIRE((*host)->run_due(start, emit).has_value());
  REQUIRE(records.size() == 2);
  CHECK(records.back().at("value").at("sequence") == 1);

  REQUIRE((*host)->run_due(start, emit).has_value());
  REQUIRE(records.size() == 3);
  CHECK(records.back().at("value").at("sequence") == 2);
}

TEST_CASE("lua host poll timeout rounds sub-millisecond deadlines up", "[lua_host_timer]") {
  using namespace std::chrono_literals;
  const auto now = gisland::LuaHost::TimePoint{};
  CHECK(gisland::lua_host_poll_timeout(std::nullopt, now) == -1);
  CHECK(gisland::lua_host_poll_timeout(now, now) == 0);
  CHECK(gisland::lua_host_poll_timeout(now + 1ns, now) == 1);
  CHECK(gisland::lua_host_poll_timeout(now + 1000001ns, now) == 2);
}

TEST_CASE("interrupted polling preserves a shared GLib and Lua deadline",
          "[lua_host_glib][lua_host_timer]") {
  using namespace std::chrono_literals;
  TemporaryDirectory temporary;
  const auto entry = temporary.path() / "interrupted-timeout.lua";
  write_file(entry, R"lua(return gisland.module {
  every = '20ms',
  update = function() gisland.data { source = 'lua' } end,
})lua");
  auto host = gisland::LuaHost::load(entry.string());
  REQUIRE(host.has_value());

  std::vector<std::string> ordering;
  const auto emit =
      [&ordering](nlohmann::json record) -> std::expected<void, gisland::LuaHostError> {
    if (record.value("type", "") == "data") {
      ordering.push_back(record.at("value").at("source").get<std::string>());
    }
    return {};
  };
  const auto started = gisland::LuaHost::Clock::now();
  REQUIRE((*host)->handle(init_record(), emit, started).has_value());

  GSource *source = g_timeout_source_new(20);
  g_source_set_callback(
      source,
      [](gpointer data) -> gboolean {
        static_cast<std::vector<std::string> *>(data)->push_back("glib");
        return G_SOURCE_REMOVE;
      },
      &ordering, nullptr);
  g_source_attach(source, nullptr);

  gisland::GlibMainContext context;
  auto query = context.prepare();
  REQUIRE(query.has_value());
  const auto now = gisland::LuaHost::Clock::now();
  const int lua_timeout = gisland::lua_host_poll_timeout((*host)->next_deadline(), now);
  const int timeout =
      query->timeout_ms < 0 ? lua_timeout : std::min(lua_timeout, query->timeout_ms);
  REQUIRE(timeout > 0);

  signal_count = 0;
  ScopedSignalHandler signal_handler{SIGUSR1};
  const pthread_t waiting_thread = pthread_self();
  std::atomic_bool stop_signals{false};
  std::atomic_bool interrupting{false};
  std::jthread interrupter{[waiting_thread, &stop_signals, &interrupting] {
    interrupting = true;
    while (!stop_signals.load()) {
      static_cast<void>(pthread_kill(waiting_thread, SIGUSR1));
      std::this_thread::sleep_for(2ms);
    }
  }};
  while (!interrupting.load()) {
    std::this_thread::yield();
  }
  std::vector<pollfd> no_descriptors;
  const auto poll_started = std::chrono::steady_clock::now();
  REQUIRE(gisland::poll_with_timeout(no_descriptors, timeout).has_value());
  const auto elapsed = std::chrono::steady_clock::now() - poll_started;
  stop_signals = true;
  interrupter.join();

  REQUIRE(signal_count >= 3);
  CHECK(elapsed < 80ms);
  REQUIRE((*host)
              ->run_external_callbacks(
                  gisland::LuaHost::Clock::now(), emit,
                  [&] { REQUIRE(context.check_and_dispatch(query->descriptors).has_value()); })
              .has_value());
  REQUIRE((*host)->run_due(gisland::LuaHost::Clock::now(), emit).has_value());
  CHECK(ordering == std::vector<std::string>{"glib", "lua"});
  g_source_unref(source);
}

TEST_CASE("lua_host_data nil periodic update emits nothing", "[lua_host_data]") {
  using namespace std::chrono_literals;
  auto host = gisland::LuaHost::load(
      (std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "nil_update.lua").string());
  REQUIRE(host.has_value());
  Records records;
  const auto emit = collect_into(records);
  const auto start = gisland::LuaHost::TimePoint{};
  REQUIRE((*host)->handle(init_record(), emit, start).has_value());
  REQUIRE((*host)->run_due(start + 1s, emit).has_value());
  CHECK(records.size() == 1);
}

TEST_CASE("lua_host_timer shutdown cancels pending callbacks", "[lua_host_timer]") {
  using namespace std::chrono_literals;
  auto host = gisland::LuaHost::load(
      (std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "data_module.lua").string());
  REQUIRE(host.has_value());
  Records records;
  const auto emit = collect_into(records);
  const auto start = gisland::LuaHost::TimePoint{};
  REQUIRE((*host)->handle(init_record(), emit, start).has_value());
  REQUIRE((*host)->handle({{"type", "shutdown"}, {"reason", "test"}, {"deadline_ms", 100}}, emit,
                          start) == gisland::LuaHostState::stopped);
  CHECK_FALSE((*host)->next_deadline().has_value());
  REQUIRE((*host)->run_due(start + 1h, emit).has_value());
  CHECK(records.size() == 2);
}

TEST_CASE("lua_host_timer and lua_host_data callback errors are fatal",
          "[lua_host_timer][lua_host_data]") {
  using namespace std::chrono_literals;
  const auto fixtures = std::filesystem::path{GISLAND_LUA_FIXTURE_DIR};
  for (const std::string_view fixture : {"timer_error.lua", "update_error.lua"}) {
    INFO(fixture);
    auto host = gisland::LuaHost::load((fixtures / fixture).string());
    REQUIRE(host.has_value());
    Records records;
    const auto emit = collect_into(records);
    const auto start = gisland::LuaHost::TimePoint{};
    REQUIRE((*host)->handle(init_record(), emit, start).has_value());
    const auto result = (*host)->run_due(start + 1ms, emit);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaHostErrorCode::callback_error);
  }
}

TEST_CASE("lua_host_lifecycle process flushes ready before lua_host_timer deferred data",
          "[lua_host_lifecycle][lua_host_timer][lua_host_data]") {
  Pipe input;
  Pipe output;
  const auto entry = std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "data_module.lua";
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    static_cast<void>(::dup2(input.read_fd(), STDIN_FILENO));
    static_cast<void>(::dup2(output.write_fd(), STDOUT_FILENO));
    ::execl(GISLAND_LUA_HOST_PATH, GISLAND_LUA_HOST_PATH, entry.c_str(), nullptr);
    _exit(127);
  }

  write_all(input.write_fd(), init_record().dump() + "\n");
  std::string text;
  for (int attempt = 0; attempt < 100 && std::ranges::count(text, '\n') < 3; ++attempt) {
    text += read_available(output.read_fd());
    ::usleep(1000);
  }
  const auto first_end = text.find('\n');
  REQUIRE(first_end != std::string::npos);
  CHECK(nlohmann::json::parse(text.substr(0, first_end)).at("type") == "ready");
  const auto second_end = text.find('\n', first_end + 1);
  REQUIRE(second_end != std::string::npos);
  CHECK(nlohmann::json::parse(text.substr(first_end + 1, second_end - first_end - 1))
            .at("value")
            .at("sequence") == 1);
  const auto third_end = text.find('\n', second_end + 1);
  REQUIRE(third_end != std::string::npos);
  CHECK(nlohmann::json::parse(text.substr(second_end + 1, third_end - second_end - 1))
            .at("value")
            .at("sequence") == 2);

  write_all(input.write_fd(),
            nlohmann::json{{"type", "shutdown"}, {"reason", "test"}, {"deadline_ms", 100}}.dump() +
                "\n");
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("lua host process does not run deferred callbacks while output is pending",
          "[lua_host_lifecycle][lua_host_timer]") {
  Pipe input;
  Pipe output;
  const auto fixture = std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "generated-saturated.lua";
  const auto marker = std::filesystem::path{GISLAND_LUA_FIXTURE_DIR} / "generated-saturated.marker";
  std::filesystem::remove(marker);
  {
    std::ofstream script{fixture};
    REQUIRE(script.good());
    script << "local marker = '" << marker.string() << "'\n"
           << R"lua(return gisland.module { init = function()
      gisland.data { payload = string.rep('x', 512 * 1024) }
      gisland.defer(function()
        local file = assert(io.open(marker, 'w'))
        file:write('ran')
        file:close()
      end)
    end })lua";
  }
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    static_cast<void>(::dup2(input.read_fd(), STDIN_FILENO));
    static_cast<void>(::dup2(output.write_fd(), STDOUT_FILENO));
    ::execl(GISLAND_LUA_HOST_PATH, GISLAND_LUA_HOST_PATH, fixture.c_str(), nullptr);
    _exit(127);
  }

  write_all(input.write_fd(), init_record().dump() + "\n");
  ::usleep(50000);
  CHECK_FALSE(std::filesystem::exists(marker));

  std::string text;
  for (int attempt = 0; attempt < 500 && !std::filesystem::exists(marker); ++attempt) {
    text += read_available(output.read_fd());
    ::usleep(1000);
  }
  CHECK(std::filesystem::exists(marker));

  write_all(input.write_fd(),
            nlohmann::json{{"type", "shutdown"}, {"reason", "test"}, {"deadline_ms", 100}}.dump() +
                "\n");
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
  std::filesystem::remove(fixture);
  std::filesystem::remove(marker);
}

TEST_CASE("lua host process pauses sustained input behind saturated output and resumes FIFO",
          "[lua_host_lifecycle][lua_host_transport]") {
  using namespace std::chrono_literals;
  TemporaryDirectory temporary;
  const auto entry = temporary.path() / "sustained-backpressure.lua";
  write_file(entry, R"lua(local sequence = 0
return gisland.module {
  init = function() gisland.data { payload = string.rep('x', 512 * 1024) } end,
  actions = {
    next = function()
      sequence = sequence + 1
      gisland.data { sequence = sequence }
      return true
    end,
  },
})lua");

  Pipe input;
  Pipe output;
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    static_cast<void>(::dup2(input.read_fd(), STDIN_FILENO));
    static_cast<void>(::dup2(output.write_fd(), STDOUT_FILENO));
    ::execl(GISLAND_LUA_HOST_PATH, GISLAND_LUA_HOST_PATH, entry.c_str(), nullptr);
    _exit(127);
  }

  write_all(input.write_fd(), init_record().dump() + "\n");
  std::this_thread::sleep_for(30ms);
  const int flags = ::fcntl(input.write_fd(), F_GETFL);
  REQUIRE(flags >= 0);
  REQUIRE(::fcntl(input.write_fd(), F_SETFL, flags & ~O_NONBLOCK) == 0);

  constexpr int action_count = 200;
  std::atomic_bool writer_ok{true};
  std::jthread writer{[&] {
    for (int sequence = 1; sequence <= action_count; ++sequence) {
      const auto record = nlohmann::json{{"type", "action"}, {"action_id", "next"}}.dump() + "\n";
      std::size_t offset = 0;
      while (offset < record.size()) {
        const auto written =
            ::write(input.write_fd(), record.data() + offset, record.size() - offset);
        if (written <= 0) {
          writer_ok = false;
          return;
        }
        offset += static_cast<std::size_t>(written);
      }
    }
  }};

  std::this_thread::sleep_for(30ms);
  int early_status = 0;
  CHECK(::waitpid(child, &early_status, WNOHANG) == 0);

  std::string text;
  constexpr int expected_records = 2 + action_count * 2;
  for (int attempt = 0; attempt < 2000 && std::ranges::count(text, '\n') < expected_records;
       ++attempt) {
    text += read_available(output.read_fd());
    std::this_thread::sleep_for(1ms);
  }
  writer.join();
  REQUIRE(writer_ok.load());
  REQUIRE(std::ranges::count(text, '\n') == expected_records);

  int expected_sequence = 1;
  std::size_t start = 0;
  for (auto end = text.find('\n'); end != std::string::npos; end = text.find('\n', start)) {
    const auto record = nlohmann::json::parse(text.substr(start, end - start));
    if (record.value("type", "") == "data" && record.at("value").contains("sequence")) {
      CHECK(record.at("value").at("sequence") == expected_sequence);
      ++expected_sequence;
    }
    start = end + 1;
  }
  CHECK(expected_sequence == action_count + 1);

  write_all(input.write_fd(),
            nlohmann::json{{"type", "shutdown"}, {"reason", "test"}, {"deadline_ms", 100}}.dump() +
                "\n");
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("lua host process advances GLib idle and timeout sources with protocol traffic",
          "[lua_host_glib]") {
  TemporaryDirectory temporary;
  const auto entry = temporary.path() / "glib-sources.lua";
  write_file(entry, R"lua(local lgi = require('lgi')
local GLib = lgi.GLib

return gisland.module {
  init = function()
    GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, function()
      gisland.data { source = 'idle' }
      return false
    end)
    GLib.timeout_add(GLib.PRIORITY_DEFAULT, 10, function()
      gisland.data { source = 'timeout' }
      return false
    end)
  end,
  actions = {
    ping = function()
      gisland.data { source = 'action' }
      return true
    end,
  },
}
)lua");

  Pipe input;
  Pipe output;
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    static_cast<void>(::dup2(input.read_fd(), STDIN_FILENO));
    static_cast<void>(::dup2(output.write_fd(), STDOUT_FILENO));
    configure_lgi_test_environment();
    ::execl(GISLAND_LUA_HOST_PATH, GISLAND_LUA_HOST_PATH, entry.c_str(), nullptr);
    _exit(127);
  }

  write_all(input.write_fd(), init_record().dump() + "\n");
  write_all(input.write_fd(),
            nlohmann::json{{"type", "action"}, {"action_id", "ping"}}.dump() + "\n");

  std::string text;
  for (int attempt = 0; attempt < 500 && std::ranges::count(text, '\n') < 5; ++attempt) {
    text += read_available(output.read_fd());
    ::usleep(1000);
  }

  write_all(input.write_fd(),
            nlohmann::json{{"type", "shutdown"}, {"reason", "test"}, {"deadline_ms", 100}}.dump() +
                "\n");
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  Records records;
  std::size_t start = 0;
  for (auto end = text.find('\n'); end != std::string::npos; end = text.find('\n', start)) {
    records.push_back(nlohmann::json::parse(text.substr(start, end - start)));
    start = end + 1;
  }
  REQUIRE(records.size() == 5);
  CHECK(records.front().at("type") == "ready");
  CHECK(std::ranges::count_if(records, [](const auto &record) {
          return record.value("type", "") == "data" && record.at("value").at("source") == "idle";
        }) == 1);
  CHECK(std::ranges::count_if(records, [](const auto &record) {
          return record.value("type", "") == "data" && record.at("value").at("source") == "timeout";
        }) == 1);
  CHECK(std::ranges::count_if(records, [](const auto &record) {
          return record.value("type", "") == "data" && record.at("value").at("source") == "action";
        }) == 1);
  CHECK(std::ranges::count_if(records, [](const auto &record) {
          return record.value("type", "") == "action_result" && record.at("action_id") == "ping";
        }) == 1);
}

TEST_CASE("lua host drains queued output before dispatching a GLib idle source",
          "[lua_host_glib]") {
  TemporaryDirectory temporary;
  const auto entry = temporary.path() / "glib-backpressure.lua";
  const auto marker = temporary.path() / "idle.marker";
  write_file(entry, "local lgi = require('lgi')\nlocal GLib = lgi.GLib\nlocal marker = " +
                        nlohmann::json(marker.string()).dump() +
                        R"lua(
return gisland.module {
  init = function()
    gisland.data { payload = string.rep('x', 512 * 1024) }
    GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, function()
      local file = assert(io.open(marker, 'w'))
      file:write('ran')
      file:close()
      return false
    end)
  end,
}
)lua");

  Pipe input;
  Pipe output;
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    static_cast<void>(::dup2(input.read_fd(), STDIN_FILENO));
    static_cast<void>(::dup2(output.write_fd(), STDOUT_FILENO));
    configure_lgi_test_environment();
    ::execl(GISLAND_LUA_HOST_PATH, GISLAND_LUA_HOST_PATH, entry.c_str(), nullptr);
    _exit(127);
  }

  write_all(input.write_fd(), init_record().dump() + "\n");
  ::usleep(50000);
  CHECK_FALSE(std::filesystem::exists(marker));

  std::string text;
  for (int attempt = 0; attempt < 500 && !std::filesystem::exists(marker); ++attempt) {
    text += read_available(output.read_fd());
    ::usleep(1000);
  }
  CHECK(std::filesystem::exists(marker));
  CHECK(text.starts_with("{\"capabilities\""));

  write_all(input.write_fd(),
            nlohmann::json{{"type", "shutdown"}, {"reason", "test"}, {"deadline_ms", 100}}.dump() +
                "\n");
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("lua host gives ready GLib sources a turn after draining saturated output",
          "[lua_host_glib][lua_host_timer]") {
  TemporaryDirectory temporary;
  const auto entry = temporary.path() / "glib-timer-fairness.lua";
  write_file(entry, R"lua(local lgi = require('lgi')
local GLib = lgi.GLib
local refill = true
local payload = string.rep('x', 256 * 1024)

local function deferred_refill()
  if refill then
    gisland.data { source = 'deferred', payload = payload }
    gisland.defer(deferred_refill)
  end
end

return gisland.module {
  every = '1ms',
  init = function()
    gisland.data { source = 'initial', payload = payload }
    gisland.defer(deferred_refill)
    GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, function()
      refill = false
      gisland.data { source = 'glib' }
      return false
    end)
  end,
  update = function()
    if refill then gisland.data { source = 'periodic', payload = payload } end
  end,
}
)lua");

  Pipe input;
  Pipe output;
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    static_cast<void>(::dup2(input.read_fd(), STDIN_FILENO));
    static_cast<void>(::dup2(output.write_fd(), STDOUT_FILENO));
    configure_lgi_test_environment();
    ::execl(GISLAND_LUA_HOST_PATH, GISLAND_LUA_HOST_PATH, entry.c_str(), nullptr);
    _exit(127);
  }

  write_all(input.write_fd(), init_record().dump() + "\n");
  std::string text;
  for (int attempt = 0; attempt < 1000 && std::ranges::count(text, '\n') < 3; ++attempt) {
    text += read_available(output.read_fd());
    ::usleep(1000);
  }

  const auto first_end = text.find('\n');
  const auto second_end =
      first_end == std::string::npos ? std::string::npos : text.find('\n', first_end + 1);
  const auto third_end =
      second_end == std::string::npos ? std::string::npos : text.find('\n', second_end + 1);
  const bool glib_dispatched_before_refill =
      third_end != std::string::npos &&
      nlohmann::json::parse(text.substr(second_end + 1, third_end - second_end - 1))
              .at("value")
              .at("source") == "glib";

  if (!glib_dispatched_before_refill) {
    REQUIRE(::kill(child, SIGTERM) == 0);
  } else {
    write_all(
        input.write_fd(),
        nlohmann::json{{"type", "shutdown"}, {"reason", "test"}, {"deadline_ms", 100}}.dump() +
            "\n");
  }
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  CHECK(glib_dispatched_before_refill);
  if (glib_dispatched_before_refill) {
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
  }
}

TEST_CASE("lua host advances an asynchronous Gio session bus source", "[lua_host_glib][dbus]") {
  TemporaryDirectory temporary;
  const auto entry = temporary.path() / "gio-bus.lua";
  write_file(entry, R"lua(local lgi = require('lgi')
local Gio = lgi.Gio

return gisland.module {
  init = function()
    Gio.bus_get(Gio.BusType.SESSION, nil, function(_, result)
      local connection = Gio.bus_get_finish(result)
      gisland.data { bus = connection ~= nil }
    end)
  end,
}
)lua");

  Pipe input;
  Pipe output;
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    static_cast<void>(::dup2(input.read_fd(), STDIN_FILENO));
    static_cast<void>(::dup2(output.write_fd(), STDOUT_FILENO));
    configure_lgi_test_environment();
    ::execlp("dbus-run-session", "dbus-run-session", "--", GISLAND_LUA_HOST_PATH, entry.c_str(),
             nullptr);
    _exit(127);
  }

  write_all(input.write_fd(), init_record().dump() + "\n");
  std::string text;
  for (int attempt = 0; attempt < 500 && std::ranges::count(text, '\n') < 2; ++attempt) {
    text += read_available(output.read_fd());
    ::usleep(1000);
  }
  const auto first_end = text.find('\n');
  REQUIRE(first_end != std::string::npos);
  CHECK(nlohmann::json::parse(text.substr(0, first_end)).at("type") == "ready");
  const auto second_end = text.find('\n', first_end + 1);
  REQUIRE(second_end != std::string::npos);
  CHECK(nlohmann::json::parse(text.substr(first_end + 1, second_end - first_end - 1)) ==
        nlohmann::json{{"type", "data"}, {"value", {{"bus", true}}}});

  write_all(input.write_fd(),
            nlohmann::json{{"type", "shutdown"}, {"reason", "test"}, {"deadline_ms", 100}}.dump() +
                "\n");
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("lua host lets shutdown remove module-owned GLib sources", "[lua_host_glib]") {
  TemporaryDirectory temporary;
  const auto entry = temporary.path() / "glib-shutdown.lua";
  const auto callback_marker = temporary.path() / "callback.marker";
  const auto shutdown_marker = temporary.path() / "shutdown.marker";
  write_file(entry,
             std::string{"local lgi = require('lgi')\nlocal GLib = lgi.GLib\n"} +
                 "local callback_marker = " + nlohmann::json(callback_marker.string()).dump() +
                 "\n" +
                 "local shutdown_marker = " + nlohmann::json(shutdown_marker.string()).dump() +
                 R"lua(
local source
return gisland.module {
  init = function()
    source = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1000, function()
      local file = assert(io.open(callback_marker, 'w'))
      file:write('ran')
      file:close()
      return false
    end)
  end,
  shutdown = function()
    GLib.Source.remove(source)
    local file = assert(io.open(shutdown_marker, 'w'))
    file:write('stopped')
    file:close()
  end,
}
)lua");

  Pipe input;
  Pipe output;
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    static_cast<void>(::dup2(input.read_fd(), STDIN_FILENO));
    static_cast<void>(::dup2(output.write_fd(), STDOUT_FILENO));
    configure_lgi_test_environment();
    ::execl(GISLAND_LUA_HOST_PATH, GISLAND_LUA_HOST_PATH, entry.c_str(), nullptr);
    _exit(127);
  }

  write_all(input.write_fd(), init_record().dump() + "\n");
  std::string text;
  for (int attempt = 0; attempt < 100 && !text.contains('\n'); ++attempt) {
    text += read_available(output.read_fd());
    ::usleep(1000);
  }
  REQUIRE(text.contains('\n'));
  write_all(input.write_fd(),
            nlohmann::json{{"type", "shutdown"}, {"reason", "test"}, {"deadline_ms", 100}}.dump() +
                "\n");

  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
  CHECK(std::filesystem::exists(shutdown_marker));
  CHECK_FALSE(std::filesystem::exists(callback_marker));
}
