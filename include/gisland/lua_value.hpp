#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <expected>
#include <string>

struct lua_State;

namespace gisland {

struct LuaValueLimits {
  static constexpr std::size_t max_depth = 16;
  static constexpr std::size_t max_items = 256;
  static constexpr std::size_t max_serialized_bytes = std::size_t{8} * 1024U * 1024U;
  static constexpr std::size_t max_string_bytes = max_serialized_bytes;
  static constexpr std::size_t max_object_key_bytes = 4096;
};

enum class LuaValueErrorCode {
  invalid_state,
  invalid_index,
  too_deep,
  too_many_items,
  string_too_long,
  invalid_utf8,
  record_too_large,
  integer_out_of_range,
  non_finite_number,
  cycle,
  mixed_table,
  sparse_table,
  unsupported_key,
  unsupported_type,
  not_a_table,
  lua_stack_error,
};

struct LuaValueError {
  LuaValueErrorCode code;
  std::string path;
  std::string message;
};

using LuaValueResult = std::expected<nlohmann::json, LuaValueError>;
using LuaPushResult = std::expected<void, LuaValueError>;

[[nodiscard]] LuaValueResult lua_value_to_json(lua_State *state, int index);
[[nodiscard]] LuaPushResult push_json_to_lua(lua_State *state, const nlohmann::json &value);

// Marks a table so an empty value converts to [] rather than the default {}.
[[nodiscard]] LuaPushResult mark_lua_table_as_array(lua_State *state, int index);

} // namespace gisland
