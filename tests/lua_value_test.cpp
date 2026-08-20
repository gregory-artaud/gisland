#include "gisland/lua_value.hpp"

#include <catch2/catch_test_macros.hpp>

#include <lua.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>

namespace {

struct LuaCloser {
  void operator()(lua_State *state) const { lua_close(state); }
};

using LuaState = std::unique_ptr<lua_State, LuaCloser>;

[[nodiscard]] LuaState make_state() { return LuaState{luaL_newstate()}; }

void check_round_trip(const nlohmann::json &value) {
  auto state = make_state();
  REQUIRE(state != nullptr);
  const auto initial_top = lua_gettop(state.get());

  const auto pushed = gisland::push_json_to_lua(state.get(), value);
  REQUIRE(pushed.has_value());
  CHECK(lua_gettop(state.get()) == initial_top + 1);

  const auto converted = gisland::lua_value_to_json(state.get(), -1);
  REQUIRE(converted.has_value());
  CHECK(*converted == value);
  CHECK(lua_gettop(state.get()) == initial_top + 1);
}

void check_lua_rejection(lua_State *state, gisland::LuaValueErrorCode code) {
  const auto top = lua_gettop(state);
  const auto result = gisland::lua_value_to_json(state, -1);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == code);
  CHECK_FALSE(result.error().path.empty());
  CHECK_FALSE(result.error().message.empty());
  CHECK(lua_gettop(state) == top);
}

void push_integer_array(lua_State *state, std::size_t size) {
  lua_createtable(state, static_cast<int>(size), 0);
  for (std::size_t index = 1; index <= size; ++index) {
    lua_pushinteger(state, static_cast<lua_Integer>(index));
    lua_rawseti(state, -2, static_cast<lua_Integer>(index));
  }
}

} // namespace

TEST_CASE("Lua values convert to JSON scalars and back") {
  check_round_trip(nullptr);
  check_round_trip(true);
  check_round_trip(std::int64_t{-9223372036854775807LL});
  check_round_trip(12.5);
  check_round_trip("bounded string");
}

TEST_CASE("Lua tables convert to JSON arrays and objects") {
  check_round_trip(nlohmann::json::array({nullptr, true, 3, "four"}));
  check_round_trip(nlohmann::json{{"enabled", true}, {"nested", nlohmann::json{{"value", 7}}}});
  check_round_trip(nlohmann::json::object());

  auto state = make_state();
  lua_newtable(state.get());
  REQUIRE(gisland::mark_lua_table_as_array(state.get(), -1).has_value());
  const auto converted = gisland::lua_value_to_json(state.get(), -1);
  REQUIRE(converted.has_value());
  CHECK(converted->is_array());
  CHECK(converted->empty());
  CHECK(lua_gettop(state.get()) == 1);
}

TEST_CASE("Lua conversion rejects cyclic mixed and sparse tables") {
  auto state = make_state();

  SECTION("cycle") {
    lua_newtable(state.get());
    lua_pushvalue(state.get(), -1);
    lua_rawseti(state.get(), -2, 1);
    check_lua_rejection(state.get(), gisland::LuaValueErrorCode::cycle);
  }

  SECTION("mixed") {
    lua_newtable(state.get());
    lua_pushinteger(state.get(), 1);
    lua_rawseti(state.get(), -2, 1);
    lua_pushboolean(state.get(), 1);
    lua_setfield(state.get(), -2, "named");
    check_lua_rejection(state.get(), gisland::LuaValueErrorCode::mixed_table);
  }

  SECTION("sparse") {
    lua_newtable(state.get());
    lua_pushinteger(state.get(), 2);
    lua_rawseti(state.get(), -2, 2);
    check_lua_rejection(state.get(), gisland::LuaValueErrorCode::sparse_table);
  }
}

TEST_CASE("Lua conversion rejects unsupported keys and values") {
  auto state = make_state();

  SECTION("key") {
    lua_newtable(state.get());
    lua_pushboolean(state.get(), 1);
    lua_pushinteger(state.get(), 1);
    lua_rawset(state.get(), -3);
    check_lua_rejection(state.get(), gisland::LuaValueErrorCode::unsupported_key);
  }

  SECTION("value") {
    lua_pushcfunction(state.get(), [](lua_State *) { return 0; });
    check_lua_rejection(state.get(), gisland::LuaValueErrorCode::unsupported_type);
  }
}

TEST_CASE("Lua conversion enforces numeric and string bounds") {
  auto state = make_state();

  SECTION("nonfinite") {
    lua_pushnumber(state.get(), std::numeric_limits<lua_Number>::infinity());
    check_lua_rejection(state.get(), gisland::LuaValueErrorCode::non_finite_number);
  }

  SECTION("string") {
    const std::string oversized(gisland::LuaValueLimits::max_string_bytes + 1, 'x');
    lua_pushlstring(state.get(), oversized.data(), oversized.size());
    check_lua_rejection(state.get(), gisland::LuaValueErrorCode::string_too_long);
  }

  SECTION("invalid UTF-8") {
    const std::string invalid{"\xff", 1};
    lua_pushlstring(state.get(), invalid.data(), invalid.size());
    check_lua_rejection(state.get(), gisland::LuaValueErrorCode::invalid_utf8);
  }
}

TEST_CASE("Lua conversion enforces depth and item bounds") {
  SECTION("depth") {
    auto state = make_state();
    lua_newtable(state.get());
    for (std::size_t depth = 0; depth < gisland::LuaValueLimits::max_depth; ++depth) {
      lua_newtable(state.get());
      lua_pushvalue(state.get(), -1);
      lua_rawseti(state.get(), -3, 1);
    }
    const auto top = lua_gettop(state.get());
    const auto result = gisland::lua_value_to_json(state.get(), 1);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaValueErrorCode::too_deep);
    CHECK(lua_gettop(state.get()) == top);
  }

  SECTION("items") {
    auto state = make_state();
    push_integer_array(state.get(), gisland::LuaValueLimits::max_items + 1);
    check_lua_rejection(state.get(), gisland::LuaValueErrorCode::too_many_items);
  }
}

TEST_CASE("Lua conversion uses an explicit item budget without changing defaults") {
  CHECK(gisland::LuaValueLimits::max_items == 256);
  auto state = make_state();
  push_integer_array(state.get(), 257);

  const auto default_result = gisland::lua_value_to_json(state.get(), -1);
  REQUIRE_FALSE(default_result.has_value());
  CHECK(default_result.error().code == gisland::LuaValueErrorCode::too_many_items);
  CHECK(default_result.error().message.find("256") != std::string::npos);

  const auto expanded_result = gisland::lua_value_to_json(
      state.get(), -1, gisland::LuaValueConversionLimits{.max_items = 512});
  REQUIRE(expanded_result.has_value());
  CHECK(expanded_result->size() == 257);

  lua_pop(state.get(), 1);
  push_integer_array(state.get(), 513);
  const auto oversized = gisland::lua_value_to_json(
      state.get(), -1, gisland::LuaValueConversionLimits{.max_items = 512});
  REQUIRE_FALSE(oversized.has_value());
  CHECK(oversized.error().code == gisland::LuaValueErrorCode::too_many_items);
  CHECK(oversized.error().message.find("512") != std::string::npos);
}

TEST_CASE("JSON conversion diagnostics report the active item budget") {
  auto state = make_state();
  const auto result = gisland::push_json_to_lua(state.get(), nlohmann::json::array({1, 2, 3}),
                                                gisland::LuaValueConversionLimits{.max_items = 2});
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == gisland::LuaValueErrorCode::too_many_items);
  CHECK(result.error().message.find("2") != std::string::npos);
  CHECK(lua_gettop(state.get()) == 0);
}

TEST_CASE("JSON to Lua conversion is bounded and stack safe on failure") {
  auto state = make_state();
  const auto initial_top = lua_gettop(state.get());

  SECTION("oversized string") {
    const auto result = gisland::push_json_to_lua(
        state.get(), std::string(gisland::LuaValueLimits::max_string_bytes + 1, 'x'));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaValueErrorCode::record_too_large);
  }

  SECTION("unsigned integer overflow") {
    const auto result =
        gisland::push_json_to_lua(state.get(), std::numeric_limits<std::uint64_t>::max());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaValueErrorCode::integer_out_of_range);
  }

  SECTION("nonfinite") {
    const nlohmann::json value = std::numeric_limits<double>::quiet_NaN();
    const auto result = gisland::push_json_to_lua(state.get(), value);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaValueErrorCode::non_finite_number);
  }

  SECTION("invalid UTF-8") {
    const nlohmann::json value = std::string{"\xff", 1};
    const auto result = gisland::push_json_to_lua(state.get(), value);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LuaValueErrorCode::invalid_utf8);
  }

  CHECK(lua_gettop(state.get()) == initial_top);
}

TEST_CASE("JSON serialized protocol records are bounded") {
  CHECK(gisland::LuaValueLimits::max_serialized_bytes == std::size_t{8} * 1024U * 1024U);
}

TEST_CASE("generic Lua JSON strings may exceed scene text limits") {
  const std::string value(64 * 1024, 'x');
  check_round_trip(value);
}
