#include "gisland/lua_host.hpp"

#include "gisland/lua_scene.hpp"
#include "gisland/lua_value.hpp"

#include <lua.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace gisland {
namespace {

constexpr std::size_t maximum_identifier_bytes = 128;
constexpr std::size_t maximum_diagnostic_bytes = 4096;
constexpr std::size_t maximum_timer_count = 256;
constexpr std::size_t maximum_buffered_output_messages = 256;
constexpr LuaValueConversionLimits data_output_limits{.max_items = 512};
// Module-to-core publish records can be 8 MiB, so this intentionally exceeds the core-to-module
// WriteQueue's 1 MiB limit.
constexpr std::size_t maximum_buffered_output_bytes = std::size_t{16} * 1024U * 1024U;

[[nodiscard]] LuaHostError error(LuaHostErrorCode code, std::string message) {
  return {code, std::move(message)};
}

[[nodiscard]] std::string lua_error_message(lua_State *state, std::string_view fallback) {
  std::size_t size = 0;
  if (const char *message = lua_tolstring(state, -1, &size); message != nullptr) {
    return std::string{message, size};
  }
  return std::string{fallback};
}

[[nodiscard]] std::expected<void, LuaHostError>
require_fields(const nlohmann::json &record, const std::set<std::string_view> &fields) {
  for (const auto &[key, value] : record.items()) {
    (void)value;
    if (!fields.contains(key)) {
      return std::unexpected(
          error(LuaHostErrorCode::protocol_error, "unknown core record field '" + key + "'"));
    }
  }
  return {};
}

[[nodiscard]] std::expected<std::string, LuaHostError> required_string(const nlohmann::json &record,
                                                                       std::string_view field) {
  const auto iterator = record.find(field);
  if (iterator == record.end() || !iterator->is_string()) {
    return std::unexpected(
        error(LuaHostErrorCode::protocol_error,
              "core record field '" + std::string{field} + "' must be a string"));
  }
  return iterator->get<std::string>();
}

[[nodiscard]] bool valid_invocation_id(const nlohmann::json &value) {
  if (!value.is_string()) {
    return false;
  }
  const auto &text = value.get_ref<const std::string &>();
  if (text.empty() || text.size() > 20) {
    return false;
  }
  std::uint64_t parsed{};
  const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), parsed);
  return code == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] std::expected<std::chrono::milliseconds, LuaHostError>
parse_timer_duration(std::string_view value) {
  std::chrono::milliseconds multiplier;
  std::size_t suffix_size = 1;
  if (value.ends_with("ms")) {
    multiplier = std::chrono::milliseconds{1};
    suffix_size = 2;
  } else if (value.ends_with('s')) {
    multiplier = std::chrono::seconds{1};
  } else if (value.ends_with('m')) {
    multiplier = std::chrono::minutes{1};
  } else if (value.ends_with('h')) {
    multiplier = std::chrono::hours{1};
  } else {
    return std::unexpected(
        error(LuaHostErrorCode::invalid_definition, "duration must use ms, s, m, or h"));
  }
  const auto number = value.substr(0, value.size() - suffix_size);
  std::uint64_t amount{};
  const auto [end, code] = std::from_chars(number.data(), number.data() + number.size(), amount);
  constexpr auto maximum =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours{24});
  const auto multiplier_count = static_cast<std::uint64_t>(multiplier.count());
  if (number.empty() || code != std::errc{} || end != number.data() + number.size() ||
      amount == 0 || amount > static_cast<std::uint64_t>(maximum.count()) / multiplier_count) {
    return std::unexpected(
        error(LuaHostErrorCode::invalid_definition, "duration must be between 1ms and 24h"));
  }
  return multiplier * amount;
}

class StackRestore {
public:
  explicit StackRestore(lua_State *state) : state_(state), top_(lua_gettop(state)) {}
  ~StackRestore() { lua_settop(state_, top_); }

  StackRestore(const StackRestore &) = delete;
  StackRestore &operator=(const StackRestore &) = delete;

private:
  lua_State *state_;
  int top_;
};

[[nodiscard]] std::expected<void, LuaHostError>
prepend_package_path(lua_State *state, const std::filesystem::path &entry_path) {
  StackRestore restore{state};
  lua_getglobal(state, "package");
  if (!lua_istable(state, -1)) {
    return std::unexpected(
        error(LuaHostErrorCode::runtime_error, "Lua package library is unavailable"));
  }
  lua_getfield(state, -1, "path");
  std::size_t inherited_size = 0;
  const char *inherited = lua_tolstring(state, -1, &inherited_size);
  if (inherited == nullptr) {
    return std::unexpected(
        error(LuaHostErrorCode::runtime_error, "Lua package.path is unavailable"));
  }
  const auto package_directory = entry_path.parent_path();
  std::string path = (package_directory / "?.lua").string();
  path.push_back(';');
  path.append((package_directory / "?/init.lua").string());
  if (inherited_size > 0) {
    path.push_back(';');
    path.append(inherited, inherited_size);
  }
  lua_pop(state, 1);
  lua_pushlstring(state, path.data(), path.size());
  lua_setfield(state, -2, "path");
  return {};
}

} // namespace

class LuaHost::Impl {
public:
  explicit Impl(lua_State *state)
      : state_(state), scene_api_([this](nlohmann::json record) {
          auto emitted = emit_record(std::move(record));
          if (!emitted) {
            return std::expected<void, std::string>{std::unexpected(emitted.error().message)};
          }
          return std::expected<void, std::string>{};
        }) {}

  ~Impl() {
    if (state_ == nullptr) {
      return;
    }
    for (const int reference : callback_references_) {
      luaL_unref(state_, LUA_REGISTRYINDEX, reference);
    }
    if (definition_reference_ != LUA_NOREF) {
      luaL_unref(state_, LUA_REGISTRYINDEX, definition_reference_);
    }
    lua_close(state_);
  }

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  [[nodiscard]] std::expected<void, std::string> register_api() {
    StackRestore restore{state_};
    lua_createtable(state_, 0, 5);

    lua_pushlightuserdata(state_, this);
    lua_pushcclosure(state_, &module_callback, 1);
    lua_setfield(state_, -2, "module");

    lua_pushcfunction(state_, &array_callback);
    lua_setfield(state_, -2, "array");

    lua_pushlightuserdata(state_, this);
    lua_pushcclosure(state_, &data_callback, 1);
    lua_setfield(state_, -2, "data");

    lua_pushlightuserdata(state_, this);
    lua_pushcclosure(state_, &defer_callback, 1);
    lua_setfield(state_, -2, "defer");

    lua_pushlightuserdata(state_, this);
    lua_pushcclosure(state_, &after_callback, 1);
    lua_setfield(state_, -2, "after");
    lua_setglobal(state_, "gisland");
    if (auto registered = scene_api_.register_into(state_); !registered) {
      return std::unexpected(std::move(registered.error()));
    }
    return {};
  }

  [[nodiscard]] std::expected<void, std::string> validate_definition(int index) {
    if (module_called_) {
      return std::unexpected("gisland.module may be called exactly once");
    }
    if (!lua_istable(state_, index)) {
      return std::unexpected("gisland.module expects one definition table");
    }

    const int table_index = lua_absindex(state_, index);
    LuaModuleDefinition candidate;
    std::vector<int> references;
    StackRestore restore{state_};

    lua_pushnil(state_);
    while (lua_next(state_, table_index) != 0) {
      if (lua_type(state_, -2) != LUA_TSTRING) {
        return std::unexpected("module field names must be strings");
      }
      std::size_t key_size = 0;
      const char *key_data = lua_tolstring(state_, -2, &key_size);
      const std::string key{key_data, key_size};
      if (!is_known_field(key)) {
        return std::unexpected("unknown module field '" + key + "'");
      }
      lua_pop(state_, 1);
    }

    lua_getfield(state_, table_index, "every");
    if (!lua_isnil(state_, -1)) {
      if (lua_type(state_, -1) != LUA_TSTRING) {
        return std::unexpected("module field 'every' must be a string");
      }
      std::size_t size = 0;
      const char *value = lua_tolstring(state_, -1, &size);
      candidate.every = std::string{value, size};
      auto duration = parse_timer_duration(*candidate.every);
      if (!duration) {
        return std::unexpected(duration.error().message);
      }
      every_ = *duration;
    }
    lua_pop(state_, 1);

    const auto retain_callback = [&](const char *name, bool &present,
                                     int &reference) -> std::expected<void, std::string> {
      lua_getfield(state_, table_index, name);
      if (lua_isnil(state_, -1)) {
        lua_pop(state_, 1);
        return {};
      }
      if (!lua_isfunction(state_, -1)) {
        lua_pop(state_, 1);
        return std::unexpected("module field '" + std::string{name} + "' must be a function");
      }
      present = true;
      reference = luaL_ref(state_, LUA_REGISTRYINDEX);
      references.push_back(reference);
      return {};
    };

    if (auto result = retain_callback("init", candidate.has_init, init_reference_); !result) {
      release_references(references);
      return result;
    }
    if (auto result = retain_callback("update", candidate.has_update, update_reference_); !result) {
      release_references(references);
      return result;
    }
    if (auto result =
            retain_callback("visibility", candidate.has_visibility, visibility_reference_);
        !result) {
      release_references(references);
      return result;
    }
    if (auto result = retain_callback("shutdown", candidate.has_shutdown, shutdown_reference_);
        !result) {
      release_references(references);
      return result;
    }
    if (auto result = retain_callback("fallback_action", candidate.has_fallback_action,
                                      fallback_action_reference_);
        !result) {
      release_references(references);
      return result;
    }

    lua_getfield(state_, table_index, "actions");
    if (!lua_isnil(state_, -1)) {
      if (!lua_istable(state_, -1)) {
        lua_pop(state_, 1);
        release_references(references);
        return std::unexpected("module field 'actions' must be a table");
      }
      const int actions_index = lua_absindex(state_, -1);
      lua_pushnil(state_);
      while (lua_next(state_, actions_index) != 0) {
        if (lua_type(state_, -2) != LUA_TSTRING) {
          lua_pop(state_, 2);
          release_references(references);
          return std::unexpected("action names must be strings");
        }
        std::size_t name_size = 0;
        const char *name_data = lua_tolstring(state_, -2, &name_size);
        const std::string name{name_data, name_size};
        if (!lua_isfunction(state_, -1)) {
          lua_pop(state_, 2);
          release_references(references);
          return std::unexpected("action '" + name + "' must be a function");
        }
        candidate.actions.push_back(name);
        const int reference = luaL_ref(state_, LUA_REGISTRYINDEX);
        action_references_.emplace(name, reference);
        references.push_back(reference);
      }
    }
    lua_pop(state_, 1);
    std::ranges::sort(candidate.actions);

    lua_pushvalue(state_, table_index);
    definition_reference_ = luaL_ref(state_, LUA_REGISTRYINDEX);
    definition_ = std::move(candidate);
    callback_references_ = std::move(references);
    module_called_ = true;
    return {};
  }

  [[nodiscard]] bool returned_registered_definition(int index) const {
    if (!module_called_ || definition_reference_ == LUA_NOREF) {
      return false;
    }
    StackRestore restore{state_};
    lua_rawgeti(state_, LUA_REGISTRYINDEX, definition_reference_);
    return lua_rawequal(state_, index, -1) != 0;
  }

  [[nodiscard]] const LuaModuleDefinition &definition() const noexcept { return definition_; }
  [[nodiscard]] std::size_t retained_callback_count() const noexcept {
    return callback_references_.size();
  }
  [[nodiscard]] int stack_size() const noexcept { return lua_gettop(state_); }
  [[nodiscard]] bool module_called() const noexcept { return module_called_; }
  [[nodiscard]] const std::string &callback_error() const noexcept { return callback_error_; }

  [[nodiscard]] std::expected<LuaHostState, LuaHostError> handle(const nlohmann::json &record,
                                                                 const Emit &emit, TimePoint now) {
    current_time_for_record_ = now;
    if (!record.is_object()) {
      return std::unexpected(
          error(LuaHostErrorCode::protocol_error, "core record must be a JSON object"));
    }
    auto type = required_string(record, "type");
    if (!type) {
      return std::unexpected(type.error());
    }
    if (stopped_) {
      if (*type == "shutdown") {
        return LuaHostState::stopped;
      }
      return std::unexpected(
          error(LuaHostErrorCode::protocol_error, "core record received after shutdown"));
    }
    if (!initialized_) {
      if (*type != "init") {
        return std::unexpected(
            error(LuaHostErrorCode::protocol_error, "first core record must be init"));
      }
      return initialize(record, emit);
    }
    if (*type == "init") {
      return std::unexpected(error(LuaHostErrorCode::protocol_error, "duplicate init record"));
    }
    if (*type == "visibility") {
      return visibility(record, emit, now);
    }
    if (*type == "shutdown") {
      return shutdown(record, emit, now);
    }
    if (*type == "action") {
      return action(record, emit, now);
    }
    return std::unexpected(
        error(LuaHostErrorCode::protocol_error, "unknown core record type '" + *type + "'"));
  }

  [[nodiscard]] bool initialized() const noexcept { return initialized_; }
  [[nodiscard]] bool stopped() const noexcept { return stopped_; }
  [[nodiscard]] std::optional<TimePoint> next_deadline() const noexcept {
    if (stopped_) {
      return std::nullopt;
    }
    std::optional<TimePoint> result = periodic_deadline_;
    for (const auto &timer : timers_) {
      if (!result || timer.deadline < *result) {
        result = timer.deadline;
      }
    }
    return result;
  }

  [[nodiscard]] std::expected<void, LuaHostError> run_due(TimePoint now, const Emit &emit) {
    if (stopped_) {
      return {};
    }
    current_emit_ = &emit;
    current_time_ = now;
    const std::uint64_t maximum_sequence = next_timer_sequence_;
    while (true) {
      auto iterator = timers_.end();
      for (auto candidate = timers_.begin(); candidate != timers_.end(); ++candidate) {
        if (candidate->deadline > now || candidate->sequence >= maximum_sequence) {
          continue;
        }
        if (iterator == timers_.end() || std::pair{candidate->deadline, candidate->sequence} <
                                             std::pair{iterator->deadline, iterator->sequence}) {
          iterator = candidate;
        }
      }
      if (iterator == timers_.end()) {
        break;
      }
      const int reference = iterator->reference;
      timers_.erase(iterator);
      auto called = invoke(reference, 0, "timer");
      luaL_unref(state_, LUA_REGISTRYINDEX, reference);
      if (!called) {
        current_emit_ = nullptr;
        return std::unexpected(called.error());
      }
    }
    if (periodic_deadline_ && *periodic_deadline_ <= now) {
      auto updated = invoke_update();
      if (!updated) {
        current_emit_ = nullptr;
        return std::unexpected(updated.error());
      }
      *periodic_deadline_ = now + *every_;
    }
    current_emit_ = nullptr;
    return {};
  }

  [[nodiscard]] std::expected<void, LuaHostError>
  run_external_callbacks(TimePoint now, const Emit &emit, const std::function<void()> &dispatch) {
    if (stopped_) {
      return {};
    }
    external_dispatch_error_.reset();
    external_dispatch_ = true;
    current_emit_ = &emit;
    current_time_ = now;
    dispatch();
    current_emit_ = nullptr;
    external_dispatch_ = false;
    if (external_dispatch_error_) {
      return std::unexpected(std::move(*external_dispatch_error_));
    }
    return {};
  }

private:
  struct Timer {
    TimePoint deadline;
    std::uint64_t sequence;
    int reference;
  };

  [[nodiscard]] std::expected<void, LuaHostError> invoke(int reference, int arguments,
                                                         std::string_view name) {
    if (reference == LUA_NOREF) {
      return {};
    }
    const int argument_start = lua_gettop(state_) - arguments + 1;
    lua_rawgeti(state_, LUA_REGISTRYINDEX, reference);
    if (arguments > 0) {
      lua_insert(state_, argument_start);
    }
    if (lua_pcall(state_, arguments, 0, 0) != LUA_OK) {
      auto message = lua_error_message(state_, "callback failed");
      lua_pop(state_, 1);
      return std::unexpected(error(LuaHostErrorCode::callback_error,
                                   std::string{name} + " callback failed: " + message));
    }
    return {};
  }

  [[nodiscard]] std::expected<LuaHostState, LuaHostError> initialize(const nlohmann::json &record,
                                                                     const Emit &emit) {
    auto known = require_fields(record, {"type", "protocol", "instance_id", "capabilities",
                                         "configuration", "locale", "timezone"});
    if (!known) {
      return std::unexpected(known.error());
    }
    const auto protocol = record.find("protocol");
    if (protocol == record.end() || !protocol->is_object()) {
      return std::unexpected(
          error(LuaHostErrorCode::protocol_error, "init protocol must be an object"));
    }
    auto protocol_fields = require_fields(*protocol, {"minimum", "maximum"});
    if (!protocol_fields) {
      return std::unexpected(protocol_fields.error());
    }
    const auto valid_version = [](const nlohmann::json &value) {
      return value.is_object() && value.size() == 2 && value.contains("major") &&
             value.contains("minor") && value.at("major").is_number_integer() &&
             value.at("minor").is_number_integer();
    };
    if (!valid_version(protocol->value("minimum", nlohmann::json{})) ||
        !valid_version(protocol->value("maximum", nlohmann::json{}))) {
      return std::unexpected(
          error(LuaHostErrorCode::protocol_error, "init protocol range is invalid"));
    }
    const auto &minimum = protocol->at("minimum");
    const auto &maximum = protocol->at("maximum");
    if (minimum.at("major").get<int>() != 1 || maximum.at("major").get<int>() != 1 ||
        minimum.at("minor").get<int>() < 0 || minimum.at("minor").get<int>() > 8 ||
        maximum.at("minor").get<int>() < 8) {
      return std::unexpected(
          error(LuaHostErrorCode::protocol_error, "core must offer protocol 1.8"));
    }
    for (const auto field : {"instance_id", "locale", "timezone"}) {
      if (!required_string(record, field)) {
        return std::unexpected(
            error(LuaHostErrorCode::protocol_error, "init string field is invalid"));
      }
    }
    const auto capabilities = record.find("capabilities");
    if (capabilities == record.end() || !capabilities->is_array()) {
      return std::unexpected(
          error(LuaHostErrorCode::protocol_error, "init capabilities must be an array"));
    }
    std::set<std::string> offered;
    for (const auto &capability : *capabilities) {
      if (!capability.is_string() || capability.get_ref<const std::string &>().empty() ||
          !offered.insert(capability.get<std::string>()).second) {
        return std::unexpected(
            error(LuaHostErrorCode::protocol_error, "init capabilities are invalid"));
      }
    }
    const auto configuration = record.find("configuration");
    if (configuration == record.end() || !configuration->is_object()) {
      return std::unexpected(
          error(LuaHostErrorCode::protocol_error, "init configuration must be an object"));
    }
    if (init_reference_ != LUA_NOREF) {
      auto pushed = push_json_to_lua(state_, *configuration);
      if (!pushed) {
        return std::unexpected(
            error(LuaHostErrorCode::value_error, std::move(pushed.error().message)));
      }
      pushed = push_json_to_lua(state_, nlohmann::json{{"instance_id", record.at("instance_id")},
                                                       {"locale", record.at("locale")},
                                                       {"timezone", record.at("timezone")}});
      if (!pushed) {
        lua_pop(state_, 1);
        return std::unexpected(
            error(LuaHostErrorCode::value_error, std::move(pushed.error().message)));
      }
      initializing_ = true;
      current_emit_ = &emit;
      current_time_ = current_time_for_record_;
      auto called = invoke(init_reference_, 2, "init");
      current_emit_ = nullptr;
      initializing_ = false;
      if (!called) {
        buffered_output_.clear();
        buffered_output_bytes_ = 0;
        return std::unexpected(called.error());
      }
    }
    static const std::array implemented_capabilities{
        "data-snapshots",      "context-images", "rich-content",
        "independent-views",   "ring-progress",  "status-indicator",
        "compact-view-styles", "icon-roles",     "progress-transitions",
    };
    std::vector<std::string> negotiated_capabilities;
    for (const auto capability : implemented_capabilities) {
      if (offered.contains(capability)) {
        negotiated_capabilities.emplace_back(capability);
      }
    }
    auto sent = emit({{"type", "ready"},
                      {"protocol_major", 1},
                      {"protocol_minor", 8},
                      {"capabilities", std::move(negotiated_capabilities)}});
    if (!sent) {
      return std::unexpected(sent.error());
    }
    for (auto &record_to_send : buffered_output_) {
      sent = emit(std::move(record_to_send));
      if (!sent) {
        return std::unexpected(sent.error());
      }
    }
    buffered_output_.clear();
    buffered_output_bytes_ = 0;
    initialized_ = true;
    if (every_) {
      periodic_deadline_ = current_time_for_record_ + *every_;
    }
    return LuaHostState::running;
  }

  [[nodiscard]] std::expected<LuaHostState, LuaHostError>
  visibility(const nlohmann::json &record, const Emit &emit, TimePoint now) {
    auto known = require_fields(record, {"type", "visibility"});
    auto value = required_string(record, "visibility");
    if (!known) {
      return std::unexpected(known.error());
    }
    if (!value ||
        (*value != "hidden" && *value != "compact-active" && *value != "expanded-active")) {
      return std::unexpected(error(LuaHostErrorCode::protocol_error, "invalid visibility value"));
    }
    if (visibility_reference_ != LUA_NOREF) {
      current_emit_ = &emit;
      current_time_ = now;
      lua_pushlstring(state_, value->data(), value->size());
      auto called = invoke(visibility_reference_, 1, "visibility");
      current_emit_ = nullptr;
      if (!called) {
        return std::unexpected(called.error());
      }
    }
    return LuaHostState::running;
  }

  [[nodiscard]] std::expected<LuaHostState, LuaHostError>
  shutdown(const nlohmann::json &record, const Emit &emit, TimePoint now) {
    auto known = require_fields(record, {"type", "reason", "deadline_ms"});
    if (!known) {
      return std::unexpected(known.error());
    }
    auto reason = required_string(record, "reason");
    const auto deadline = record.find("deadline_ms");
    if (!reason || deadline == record.end() || !deadline->is_number_integer() ||
        deadline->get<std::int64_t>() < 0) {
      return std::unexpected(error(LuaHostErrorCode::protocol_error, "invalid shutdown record"));
    }
    stopped_ = true;
    current_emit_ = &emit;
    current_time_ = now;
    auto called = invoke(shutdown_reference_, 0, "shutdown");
    current_emit_ = nullptr;
    for (const auto &timer : timers_) {
      luaL_unref(state_, LUA_REGISTRYINDEX, timer.reference);
    }
    timers_.clear();
    periodic_deadline_.reset();
    if (!called) {
      return std::unexpected(called.error());
    }
    return LuaHostState::stopped;
  }

  [[nodiscard]] std::expected<LuaHostState, LuaHostError> action(const nlohmann::json &record,
                                                                 const Emit &emit, TimePoint now) {
    auto known = require_fields(record, {"type", "action_id", "value", "invocation_id"});
    auto action_id = required_string(record, "action_id");
    if (!known) {
      return std::unexpected(known.error());
    }
    if (!action_id || action_id->empty() || action_id->size() > maximum_identifier_bytes) {
      return std::unexpected(error(LuaHostErrorCode::protocol_error, "invalid action ID"));
    }
    const auto invocation = record.find("invocation_id");
    if (invocation != record.end() && !valid_invocation_id(*invocation)) {
      return std::unexpected(
          error(LuaHostErrorCode::protocol_error, "invalid action invocation ID"));
    }

    const auto handler = action_references_.find(*action_id);
    const bool fallback =
        handler == action_references_.end() && fallback_action_reference_ != LUA_NOREF;
    if (handler == action_references_.end() && !fallback) {
      return reject_action(*action_id, invocation == record.end() ? nullptr : &*invocation,
                           "unknown action", emit);
    }

    StackRestore restore{state_};
    if (fallback) {
      lua_pushlstring(state_, action_id->data(), action_id->size());
    }
    const auto value = record.find("value");
    if (value == record.end()) {
      lua_pushnil(state_);
    } else if (auto pushed = push_json_to_lua(state_, *value); !pushed) {
      return std::unexpected(
          error(LuaHostErrorCode::value_error, std::move(pushed.error().message)));
    }

    const int arguments = fallback ? 2 : 1;
    const int argument_index = lua_gettop(state_) - arguments + 1;
    lua_rawgeti(state_, LUA_REGISTRYINDEX, fallback ? fallback_action_reference_ : handler->second);
    lua_insert(state_, argument_index);
    current_emit_ = &emit;
    current_time_ = now;
    const int status = lua_pcall(state_, arguments, LUA_MULTRET, 0);
    current_emit_ = nullptr;
    if (status != LUA_OK) {
      const auto message = lua_error_message(state_, "action callback failed");
      return reject_action(*action_id, invocation == record.end() ? nullptr : &*invocation,
                           bounded_diagnostic("action callback failed: " + message), emit);
    }

    const int returns = lua_gettop(state_);
    if (returns == 1 && lua_isboolean(state_, 1)) {
      return emit_action_result(*action_id, lua_toboolean(state_, 1) != 0, std::nullopt,
                                invocation == record.end() ? nullptr : &*invocation, emit);
    }
    if (returns == 2 && lua_isboolean(state_, 1) && lua_toboolean(state_, 1) == 0 &&
        lua_type(state_, 2) == LUA_TSTRING) {
      std::size_t size = 0;
      const char *data = lua_tolstring(state_, 2, &size);
      if (size <= maximum_diagnostic_bytes) {
        return emit_action_result(*action_id, false, std::string{data, size},
                                  invocation == record.end() ? nullptr : &*invocation, emit);
      }
    }
    return reject_action(*action_id, invocation == record.end() ? nullptr : &*invocation,
                         "invalid action callback return", emit);
  }

  [[nodiscard]] std::expected<LuaHostState, LuaHostError>
  reject_action(const std::string &action_id, const nlohmann::json *invocation,
                std::string diagnostic, const Emit &emit) {
    diagnostic = bounded_diagnostic(std::move(diagnostic));
    auto logged = emit({{"type", "log"}, {"level", "error"}, {"message", diagnostic}});
    if (!logged) {
      return std::unexpected(logged.error());
    }
    return emit_action_result(action_id, false, diagnostic, invocation, emit);
  }

  [[nodiscard]] std::expected<LuaHostState, LuaHostError>
  emit_action_result(const std::string &action_id, bool accepted,
                     std::optional<std::string> message, const nlohmann::json *invocation,
                     const Emit &emit) {
    nlohmann::json result{
        {"type", "action_result"}, {"action_id", action_id}, {"accepted", accepted}};
    if (message) {
      result["message"] = std::move(*message);
    }
    if (invocation != nullptr) {
      result["invocation_id"] = *invocation;
    }
    auto sent = emit(std::move(result));
    if (!sent) {
      return std::unexpected(sent.error());
    }
    return LuaHostState::running;
  }

  [[nodiscard]] static std::string bounded_diagnostic(std::string diagnostic) {
    if (diagnostic.size() > maximum_diagnostic_bytes) {
      diagnostic.resize(maximum_diagnostic_bytes);
    }
    return diagnostic;
  }

  static int module_callback(lua_State *state) {
    auto *impl = static_cast<Impl *>(lua_touserdata(state, lua_upvalueindex(1)));
    if (lua_gettop(state) != 1) {
      lua_pushliteral(state, "gisland.module expects one definition table");
      return lua_error(state);
    }
    {
      auto result = impl->validate_definition(1);
      if (result) {
        lua_pushvalue(state, 1);
        return 1;
      }
      impl->callback_error_ = std::move(result.error());
    }
    lua_pushlstring(state, impl->callback_error_.data(), impl->callback_error_.size());
    return lua_error(state);
  }

  static int array_callback(lua_State *state) {
    if (lua_gettop(state) != 0) {
      lua_pushliteral(state, "gisland.array expects no arguments");
      return lua_error(state);
    }
    lua_newtable(state);
    {
      const auto result = mark_lua_table_as_array(state, -1);
      if (result) {
        return 1;
      }
    }
    lua_pushliteral(state, "cannot create marked Lua array");
    return lua_error(state);
  }

  static int data_callback(lua_State *state) {
    auto *impl = static_cast<Impl *>(lua_touserdata(state, lua_upvalueindex(1)));
    if (lua_gettop(state) != 1) {
      lua_pushliteral(state, "gisland.data expects one value");
      return lua_error(state);
    }
    bool failed = false;
    {
      auto value = lua_value_to_json(state, 1, data_output_limits);
      if (!value) {
        impl->callback_error_ = value.error().message;
        failed = true;
      } else if (!value->is_object()) {
        lua_pushliteral(state, "gisland.data expects an object value");
        failed = true;
      } else {
        auto sent = impl->emit_record({{"type", "data"}, {"value", std::move(*value)}});
        if (!sent) {
          impl->callback_error_ = sent.error().message;
          failed = true;
        }
      }
    }
    if (failed) {
      lua_pushlstring(state, impl->callback_error_.data(), impl->callback_error_.size());
      return lua_error(state);
    }
    return 0;
  }

  static int defer_callback(lua_State *state) {
    auto *impl = static_cast<Impl *>(lua_touserdata(state, lua_upvalueindex(1)));
    return impl->schedule_timer(state, std::chrono::milliseconds{0}, 1, "gisland.defer");
  }

  static int after_callback(lua_State *state) {
    auto *impl = static_cast<Impl *>(lua_touserdata(state, lua_upvalueindex(1)));
    if (lua_gettop(state) != 2 || lua_type(state, 1) != LUA_TSTRING) {
      lua_pushliteral(state, "gisland.after expects a duration and callback");
      return lua_error(state);
    }
    std::size_t size = 0;
    const char *text = lua_tolstring(state, 1, &size);
    std::chrono::milliseconds duration{};
    bool failed = false;
    {
      auto parsed = parse_timer_duration(std::string_view{text, size});
      if (!parsed) {
        impl->callback_error_ = parsed.error().message;
        failed = true;
      } else {
        duration = *parsed;
      }
    }
    if (failed) {
      lua_pushlstring(state, impl->callback_error_.data(), impl->callback_error_.size());
      return lua_error(state);
    }
    return impl->schedule_timer(state, duration, 2, "gisland.after");
  }

  int schedule_timer(lua_State *state, std::chrono::milliseconds duration, int callback_index,
                     const char *name) {
    if (lua_gettop(state) != callback_index || !lua_isfunction(state, callback_index)) {
      lua_pushfstring(state, "%s expects a callback", name);
      return lua_error(state);
    }
    if (timers_.size() >= maximum_timer_count) {
      lua_pushliteral(state, "timer queue limit exceeded");
      return lua_error(state);
    }
    lua_pushvalue(state, callback_index);
    const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
    timers_.push_back(Timer{current_time_ + duration, next_timer_sequence_++, reference});
    return 0;
  }

  [[nodiscard]] std::expected<void, LuaHostError> emit_record(nlohmann::json record) {
    if (initializing_) {
      std::size_t serialized_bytes = 0;
      try {
        serialized_bytes = record.dump().size() + 1U;
      } catch (const nlohmann::json::exception &) {
        return std::unexpected(
            error(LuaHostErrorCode::output_error, "output record cannot be serialized"));
      }
      if (buffered_output_.size() >= maximum_buffered_output_messages ||
          serialized_bytes > maximum_buffered_output_bytes - buffered_output_bytes_) {
        return std::unexpected(
            error(LuaHostErrorCode::output_error, "output queue limit exceeded"));
      }
      buffered_output_.push_back(std::move(record));
      buffered_output_bytes_ += serialized_bytes;
      return {};
    }
    if (current_emit_ == nullptr) {
      return std::unexpected(
          error(LuaHostErrorCode::output_error, "Lua API called outside host dispatch"));
    }
    auto emitted = (*current_emit_)(std::move(record));
    if (!emitted && external_dispatch_ && !external_dispatch_error_) {
      external_dispatch_error_ = emitted.error();
    }
    return emitted;
  }

  [[nodiscard]] std::expected<void, LuaHostError> invoke_update() {
    if (update_reference_ == LUA_NOREF) {
      return {};
    }
    lua_rawgeti(state_, LUA_REGISTRYINDEX, update_reference_);
    if (lua_pcall(state_, 0, 1, 0) != LUA_OK) {
      auto message = lua_error_message(state_, "update callback failed");
      lua_pop(state_, 1);
      return std::unexpected(
          error(LuaHostErrorCode::callback_error, "update callback failed: " + message));
    }
    if (lua_isnil(state_, -1)) {
      lua_pop(state_, 1);
      return {};
    }
    auto value = lua_value_to_json(state_, -1);
    lua_pop(state_, 1);
    if (!value) {
      return std::unexpected(
          error(LuaHostErrorCode::value_error, std::move(value.error().message)));
    }
    if (!value->is_object()) {
      return std::unexpected(
          error(LuaHostErrorCode::value_error, "update callback must return an object or nil"));
    }
    return emit_record({{"type", "data"}, {"value", std::move(*value)}});
  }

  [[nodiscard]] static bool is_known_field(std::string_view field) {
    constexpr std::array fields{"every",           "init",       "update",  "actions",
                                "fallback_action", "visibility", "shutdown"};
    return std::ranges::find(fields, field) != fields.end();
  }

  void release_references(const std::vector<int> &references) {
    for (const int reference : references) {
      luaL_unref(state_, LUA_REGISTRYINDEX, reference);
    }
  }

  lua_State *state_{};
  LuaSceneApi scene_api_;
  LuaModuleDefinition definition_;
  std::vector<int> callback_references_;
  std::unordered_map<std::string, int> action_references_;
  int definition_reference_{LUA_NOREF};
  int init_reference_{LUA_NOREF};
  int update_reference_{LUA_NOREF};
  int visibility_reference_{LUA_NOREF};
  int shutdown_reference_{LUA_NOREF};
  int fallback_action_reference_{LUA_NOREF};
  bool module_called_{};
  bool initialized_{};
  bool stopped_{};
  std::string callback_error_;
  std::optional<LuaHostError> external_dispatch_error_;
  std::optional<std::chrono::milliseconds> every_;
  std::optional<TimePoint> periodic_deadline_;
  std::vector<Timer> timers_;
  std::vector<nlohmann::json> buffered_output_;
  std::size_t buffered_output_bytes_{};
  const Emit *current_emit_{};
  bool external_dispatch_{};
  TimePoint current_time_{};
  TimePoint current_time_for_record_{};
  std::uint64_t next_timer_sequence_{};
  bool initializing_{};
};

std::expected<std::unique_ptr<LuaHost>, LuaHostError> LuaHost::load(const std::string &entry_path) {
  std::ifstream input{entry_path, std::ios::binary};
  if (!input) {
    return std::unexpected(error(LuaHostErrorCode::file_error, "cannot load Lua entry file"));
  }
  std::string source{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
  if (input.bad()) {
    return std::unexpected(error(LuaHostErrorCode::file_error, "cannot read Lua entry file"));
  }
  return load(entry_path, std::move(source));
}

std::expected<std::unique_ptr<LuaHost>, LuaHostError> LuaHost::load(const std::string &entry_path,
                                                                    std::string entry_source) {
  lua_State *state = luaL_newstate();
  if (state == nullptr) {
    return std::unexpected(
        error(LuaHostErrorCode::state_creation_failed, "cannot create Lua state"));
  }
  auto impl = std::make_unique<Impl>(state);
  luaL_openlibs(state);
  if (auto path = prepend_package_path(state, entry_path); !path) {
    return std::unexpected(std::move(path.error()));
  }
  if (auto registered = impl->register_api(); !registered) {
    return std::unexpected(error(LuaHostErrorCode::runtime_error, std::move(registered.error())));
  }

  const int load_status =
      luaL_loadbuffer(state, entry_source.data(), entry_source.size(), entry_path.c_str());
  if (load_status != LUA_OK) {
    const auto code = load_status == LUA_ERRSYNTAX ? LuaHostErrorCode::syntax_error
                                                   : LuaHostErrorCode::file_error;
    return std::unexpected(error(code, lua_error_message(state, "cannot load Lua entry file")));
  }
  if (lua_pcall(state, 0, LUA_MULTRET, 0) != LUA_OK) {
    const auto code = impl->callback_error().empty() ? LuaHostErrorCode::runtime_error
                                                     : LuaHostErrorCode::invalid_definition;
    return std::unexpected(error(code, lua_error_message(state, "Lua entry failed")));
  }
  if (!impl->module_called()) {
    return std::unexpected(error(LuaHostErrorCode::missing_definition,
                                 "Lua entry must return one definition created by gisland.module"));
  }
  if (lua_gettop(state) != 1 || !impl->returned_registered_definition(1)) {
    return std::unexpected(error(LuaHostErrorCode::invalid_return,
                                 "Lua entry must return the definition created by gisland.module"));
  }
  lua_pop(state, 1);
  return std::unique_ptr<LuaHost>{new LuaHost{std::move(impl)}};
}

std::expected<std::chrono::milliseconds, LuaHostError>
LuaHost::parse_duration(std::string_view value) {
  return parse_timer_duration(value);
}

LuaHost::LuaHost(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LuaHost::~LuaHost() = default;
LuaHost::LuaHost(LuaHost &&) noexcept = default;
LuaHost &LuaHost::operator=(LuaHost &&) noexcept = default;

const LuaModuleDefinition &LuaHost::definition() const noexcept { return impl_->definition(); }

std::size_t LuaHost::retained_callback_count() const noexcept {
  return impl_->retained_callback_count();
}

int LuaHost::stack_size() const noexcept { return impl_->stack_size(); }

std::expected<LuaHostState, LuaHostError> LuaHost::handle(const nlohmann::json &record,
                                                          const Emit &emit, TimePoint now) {
  return impl_->handle(record, emit, now);
}

bool LuaHost::initialized() const noexcept { return impl_->initialized(); }

bool LuaHost::stopped() const noexcept { return impl_->stopped(); }

std::optional<LuaHost::TimePoint> LuaHost::next_deadline() const noexcept {
  return impl_->next_deadline();
}

std::expected<void, LuaHostError> LuaHost::run_due(TimePoint now, const Emit &emit) {
  return impl_->run_due(now, emit);
}

std::expected<void, LuaHostError>
LuaHost::run_external_callbacks(TimePoint now, const Emit &emit,
                                const std::function<void()> &dispatch) {
  return impl_->run_external_callbacks(now, emit, dispatch);
}

int lua_host_poll_timeout(std::optional<LuaHost::TimePoint> deadline, LuaHost::TimePoint now) {
  if (!deadline) {
    return -1;
  }
  if (*deadline <= now) {
    return 0;
  }
  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(*deadline - now).count();
  return static_cast<int>(std::min<std::int64_t>(remaining, std::numeric_limits<int>::max()));
}

} // namespace gisland
