#include "gisland/lua_value.hpp"

#include <lua.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace gisland {
namespace {

char array_marker_registry_key;
char json_null_sentinel;

class StackRestore {
public:
  explicit StackRestore(lua_State *state) : state_(state), top_(lua_gettop(state)) {}
  ~StackRestore() {
    if (active_) {
      lua_settop(state_, top_);
    }
  }

  StackRestore(const StackRestore &) = delete;
  StackRestore &operator=(const StackRestore &) = delete;

  void release() { active_ = false; }

private:
  lua_State *state_;
  int top_;
  bool active_{true};
};

[[nodiscard]] LuaValueError error(LuaValueErrorCode code, std::string path, std::string message) {
  if (path.empty()) {
    path = "/";
  }
  return LuaValueError{code, std::move(path), std::move(message)};
}

[[nodiscard]] std::string path_member(std::string_view path, std::string_view member) {
  std::string result{path};
  result.push_back('/');
  for (const char character : member) {
    if (character == '~') {
      result += "~0";
    } else if (character == '/') {
      result += "~1";
    } else {
      result.push_back(character);
    }
  }
  return result;
}

[[nodiscard]] std::string path_index(std::string_view path, lua_Integer index) {
  return std::string{path} + '/' + std::to_string(index - 1);
}

void push_array_marker_table(lua_State *state) {
  lua_rawgetp(state, LUA_REGISTRYINDEX, &array_marker_registry_key);
  if (lua_istable(state, -1)) {
    return;
  }

  lua_pop(state, 1);
  lua_newtable(state);
  lua_newtable(state);
  lua_pushliteral(state, "k");
  lua_setfield(state, -2, "__mode");
  lua_setmetatable(state, -2);
  lua_pushvalue(state, -1);
  lua_rawsetp(state, LUA_REGISTRYINDEX, &array_marker_registry_key);
}

[[nodiscard]] bool is_marked_array(lua_State *state, int index) {
  const int absolute_index = lua_absindex(state, index);
  push_array_marker_table(state);
  lua_pushvalue(state, absolute_index);
  lua_rawget(state, -2);
  const bool marked = lua_toboolean(state, -1) != 0;
  lua_pop(state, 2);
  return marked;
}

struct ConversionContext {
  LuaValueConversionLimits limits;
  std::size_t items{};
  std::unordered_set<const void *> active_tables;
};

[[nodiscard]] std::expected<std::size_t, LuaValueError>
serialized_size(const nlohmann::json &value) {
  try {
    return value.dump().size();
  } catch (const nlohmann::json::type_error &) {
    return std::unexpected(error(LuaValueErrorCode::invalid_utf8, "/",
                                 "JSON strings and object keys must contain valid UTF-8"));
  }
}

class ActiveTable {
public:
  ActiveTable(ConversionContext &context, const void *identity)
      : context_(context), identity_(identity) {
    inserted_ = context_.active_tables.insert(identity_).second;
  }
  ~ActiveTable() {
    if (inserted_) {
      context_.active_tables.erase(identity_);
    }
  }

  [[nodiscard]] bool inserted() const { return inserted_; }

private:
  ConversionContext &context_;
  const void *identity_;
  bool inserted_{};
};

[[nodiscard]] LuaValueResult convert_lua_value(lua_State *state, int index, std::size_t depth,
                                               std::string path, ConversionContext &context);

[[nodiscard]] LuaValueResult convert_lua_table(lua_State *state, int index, std::size_t depth,
                                               std::string path, ConversionContext &context) {
  if (depth >= LuaValueLimits::max_depth) {
    return std::unexpected(
        error(LuaValueErrorCode::too_deep, path, "Lua value exceeds maximum depth of 16"));
  }

  const int table_index = lua_absindex(state, index);
  ActiveTable active{context, lua_topointer(state, table_index)};
  if (!active.inserted()) {
    return std::unexpected(error(LuaValueErrorCode::cycle, path, "Lua table contains a cycle"));
  }

  enum class TableKind { empty, array, object };
  TableKind kind = TableKind::empty;
  lua_Integer largest_index = 0;
  std::size_t entry_count = 0;
  nlohmann::json object = nlohmann::json::object();

  lua_pushnil(state);
  while (lua_next(state, table_index) != 0) {
    ++context.items;
    ++entry_count;
    if (context.items > context.limits.max_items) {
      lua_pop(state, 2);
      return std::unexpected(error(LuaValueErrorCode::too_many_items, path,
                                   "Lua value exceeds maximum item count of " +
                                       std::to_string(context.limits.max_items)));
    }

    if (lua_type(state, -2) == LUA_TNUMBER && lua_isinteger(state, -2) != 0) {
      if (kind == TableKind::object) {
        lua_pop(state, 2);
        return std::unexpected(
            error(LuaValueErrorCode::mixed_table, path, "Lua table mixes array and object keys"));
      }
      kind = TableKind::array;
      const lua_Integer key = lua_tointeger(state, -2);
      if (key < 1) {
        lua_pop(state, 2);
        return std::unexpected(error(LuaValueErrorCode::sparse_table, path,
                                     "Lua array keys must be contiguous and start at 1"));
      }
      largest_index = std::max(largest_index, key);
    } else if (lua_type(state, -2) == LUA_TSTRING) {
      if (kind == TableKind::array) {
        lua_pop(state, 2);
        return std::unexpected(
            error(LuaValueErrorCode::mixed_table, path, "Lua table mixes array and object keys"));
      }
      kind = TableKind::object;
      std::size_t key_size = 0;
      const char *key_data = lua_tolstring(state, -2, &key_size);
      if (key_size > LuaValueLimits::max_object_key_bytes) {
        lua_pop(state, 2);
        return std::unexpected(
            error(LuaValueErrorCode::string_too_long, path, "Lua object key exceeds 4096 bytes"));
      }
      const std::string key{key_data, key_size};
      auto converted = convert_lua_value(state, -1, depth + 1, path_member(path, key), context);
      if (!converted) {
        lua_pop(state, 2);
        return converted;
      }
      object[std::move(key)] = std::move(*converted);
    } else {
      lua_pop(state, 2);
      return std::unexpected(
          error(LuaValueErrorCode::unsupported_key, path, "Lua object keys must be strings"));
    }
    lua_pop(state, 1);
  }

  if (kind == TableKind::empty) {
    return is_marked_array(state, table_index) ? nlohmann::json::array() : nlohmann::json::object();
  }
  if (kind == TableKind::object) {
    return object;
  }
  if (largest_index != static_cast<lua_Integer>(entry_count)) {
    return std::unexpected(error(LuaValueErrorCode::sparse_table, path,
                                 "Lua array keys must be contiguous and start at 1"));
  }

  nlohmann::json array = nlohmann::json::array();
  for (lua_Integer array_index = 1; array_index <= largest_index; ++array_index) {
    lua_rawgeti(state, table_index, array_index);
    auto converted =
        convert_lua_value(state, -1, depth + 1, path_index(path, array_index), context);
    lua_pop(state, 1);
    if (!converted) {
      return converted;
    }
    array.push_back(std::move(*converted));
  }
  return array;
}

[[nodiscard]] LuaValueResult convert_lua_value(lua_State *state, int index, std::size_t depth,
                                               std::string path, ConversionContext &context) {
  switch (lua_type(state, index)) {
  case LUA_TNIL:
    return nullptr;
  case LUA_TBOOLEAN:
    return lua_toboolean(state, index) != 0;
  case LUA_TNUMBER:
    if (lua_isinteger(state, index) != 0) {
      const lua_Integer value = lua_tointeger(state, index);
      if constexpr (sizeof(lua_Integer) > sizeof(std::int64_t)) {
        if (value < static_cast<lua_Integer>(std::numeric_limits<std::int64_t>::min()) ||
            value > static_cast<lua_Integer>(std::numeric_limits<std::int64_t>::max())) {
          return std::unexpected(error(LuaValueErrorCode::integer_out_of_range, path,
                                       "Lua integer does not fit int64"));
        }
      }
      return static_cast<std::int64_t>(value);
    }
    if (const lua_Number value = lua_tonumber(state, index); std::isfinite(value)) {
      return static_cast<double>(value);
    }
    return std::unexpected(
        error(LuaValueErrorCode::non_finite_number, path, "Lua numbers must be finite"));
  case LUA_TSTRING: {
    std::size_t size = 0;
    const char *data = lua_tolstring(state, index, &size);
    if (size > LuaValueLimits::max_string_bytes) {
      return std::unexpected(
          error(LuaValueErrorCode::string_too_long, path, "Lua string exceeds 8 MiB"));
    }
    return std::string{data, size};
  }
  case LUA_TTABLE:
    return convert_lua_table(state, index, depth, std::move(path), context);
  case LUA_TLIGHTUSERDATA:
    if (lua_touserdata(state, index) == &json_null_sentinel) {
      return nullptr;
    }
    return std::unexpected(
        error(LuaValueErrorCode::unsupported_type, path, "Lua value type is not JSON-compatible"));
  case LUA_TNONE:
    return std::unexpected(
        error(LuaValueErrorCode::invalid_index, path, "Lua stack index is invalid"));
  default:
    return std::unexpected(
        error(LuaValueErrorCode::unsupported_type, path, "Lua value type is not JSON-compatible"));
  }
}

struct JsonContext {
  LuaValueConversionLimits limits;
  std::size_t items{};
};

[[nodiscard]] LuaPushResult push_json_value(lua_State *state, const nlohmann::json &value,
                                            std::size_t depth, std::string path,
                                            JsonContext &context, bool nested) {
  if ((value.is_array() || value.is_object()) && depth >= LuaValueLimits::max_depth) {
    return std::unexpected(
        error(LuaValueErrorCode::too_deep, path, "JSON value exceeds maximum depth of 16"));
  }
  if (value.is_null()) {
    if (nested) {
      lua_pushlightuserdata(state, &json_null_sentinel);
    } else {
      lua_pushnil(state);
    }
  } else if (value.is_boolean()) {
    lua_pushboolean(state, value.get<bool>() ? 1 : 0);
  } else if (value.is_number_unsigned()) {
    const auto integer = value.get<std::uint64_t>();
    if (integer > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        integer > static_cast<std::uint64_t>(std::numeric_limits<lua_Integer>::max())) {
      return std::unexpected(
          error(LuaValueErrorCode::integer_out_of_range, path, "JSON integer does not fit int64"));
    }
    lua_pushinteger(state, static_cast<lua_Integer>(integer));
  } else if (value.is_number_integer()) {
    const auto integer = value.get<std::int64_t>();
    if constexpr (sizeof(lua_Integer) < sizeof(std::int64_t)) {
      if (integer < static_cast<std::int64_t>(std::numeric_limits<lua_Integer>::min()) ||
          integer > static_cast<std::int64_t>(std::numeric_limits<lua_Integer>::max())) {
        return std::unexpected(error(LuaValueErrorCode::integer_out_of_range, path,
                                     "JSON integer does not fit Lua integer"));
      }
    }
    lua_pushinteger(state, static_cast<lua_Integer>(integer));
  } else if (value.is_number_float()) {
    const double number = value.get<double>();
    if (!std::isfinite(number)) {
      return std::unexpected(
          error(LuaValueErrorCode::non_finite_number, path, "JSON numbers must be finite"));
    }
    lua_pushnumber(state, static_cast<lua_Number>(number));
  } else if (value.is_string()) {
    const auto &string = value.get_ref<const std::string &>();
    if (string.size() > LuaValueLimits::max_string_bytes) {
      return std::unexpected(
          error(LuaValueErrorCode::string_too_long, path, "JSON string exceeds 8 MiB"));
    }
    lua_pushlstring(state, string.data(), string.size());
  } else if (value.is_array()) {
    lua_createtable(state, static_cast<int>(value.size()), 0);
    const int table_index = lua_absindex(state, -1);
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (++context.items > context.limits.max_items) {
        return std::unexpected(error(LuaValueErrorCode::too_many_items, path,
                                     "JSON value exceeds maximum item count of " +
                                         std::to_string(context.limits.max_items)));
      }
      auto result = push_json_value(state, value[index], depth + 1,
                                    std::string{path} + '/' + std::to_string(index), context, true);
      if (!result) {
        return result;
      }
      lua_rawseti(state, table_index, static_cast<lua_Integer>(index + 1));
    }
    if (value.empty()) {
      auto marked = mark_lua_table_as_array(state, table_index);
      if (!marked) {
        return marked;
      }
    }
  } else if (value.is_object()) {
    lua_createtable(state, 0, static_cast<int>(value.size()));
    const int table_index = lua_absindex(state, -1);
    for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
      if (++context.items > context.limits.max_items) {
        return std::unexpected(error(LuaValueErrorCode::too_many_items, path,
                                     "JSON value exceeds maximum item count of " +
                                         std::to_string(context.limits.max_items)));
      }
      if (iterator.key().size() > LuaValueLimits::max_object_key_bytes) {
        return std::unexpected(
            error(LuaValueErrorCode::string_too_long, path, "JSON object key exceeds 4096 bytes"));
      }
      lua_pushlstring(state, iterator.key().data(), iterator.key().size());
      auto result = push_json_value(state, iterator.value(), depth + 1,
                                    path_member(path, iterator.key()), context, true);
      if (!result) {
        return result;
      }
      lua_rawset(state, table_index);
    }
  } else {
    return std::unexpected(
        error(LuaValueErrorCode::unsupported_type, path, "JSON value type is not supported"));
  }
  return {};
}

} // namespace

LuaValueResult lua_value_to_json(lua_State *state, int index, LuaValueConversionLimits limits) {
  if (state == nullptr) {
    return std::unexpected(
        error(LuaValueErrorCode::invalid_state, "/", "Lua state must not be null"));
  }
  StackRestore restore{state};
  if (lua_type(state, index) == LUA_TNONE) {
    return std::unexpected(
        error(LuaValueErrorCode::invalid_index, "/", "Lua stack index is invalid"));
  }

  ConversionContext context{.limits = limits, .items = 0, .active_tables = {}};
  auto result = convert_lua_value(state, index, 0, "", context);
  if (!result) {
    return result;
  }
  const auto size = serialized_size(*result);
  if (!size) {
    return std::unexpected(size.error());
  }
  if (*size > LuaValueLimits::max_serialized_bytes) {
    return std::unexpected(
        error(LuaValueErrorCode::record_too_large, "/", "Serialized JSON exceeds 8 MiB"));
  }
  return result;
}

LuaPushResult push_json_to_lua(lua_State *state, const nlohmann::json &value,
                               LuaValueConversionLimits limits) {
  if (state == nullptr) {
    return std::unexpected(
        error(LuaValueErrorCode::invalid_state, "/", "Lua state must not be null"));
  }
  StackRestore restore{state};
  const auto size = serialized_size(value);
  if (!size) {
    return std::unexpected(size.error());
  }
  if (*size > LuaValueLimits::max_serialized_bytes) {
    return std::unexpected(
        error(LuaValueErrorCode::record_too_large, "/", "Serialized JSON exceeds 8 MiB"));
  }
  if (!lua_checkstack(state, static_cast<int>(LuaValueLimits::max_depth * 3U))) {
    return std::unexpected(
        error(LuaValueErrorCode::lua_stack_error, "/", "Lua stack cannot grow for conversion"));
  }

  JsonContext context{.limits = limits};
  auto result = push_json_value(state, value, 0, "", context, false);
  if (!result) {
    return result;
  }
  restore.release();
  return {};
}

LuaPushResult mark_lua_table_as_array(lua_State *state, int index) {
  if (state == nullptr) {
    return std::unexpected(
        error(LuaValueErrorCode::invalid_state, "/", "Lua state must not be null"));
  }
  StackRestore restore{state};
  const int table_index = lua_absindex(state, index);
  if (!lua_istable(state, table_index)) {
    return std::unexpected(
        error(LuaValueErrorCode::not_a_table, "/", "Only Lua tables can be marked as arrays"));
  }
  if (!lua_checkstack(state, 3)) {
    return std::unexpected(
        error(LuaValueErrorCode::lua_stack_error, "/", "Lua stack cannot grow for array marker"));
  }
  push_array_marker_table(state);
  lua_pushvalue(state, table_index);
  lua_pushboolean(state, 1);
  lua_rawset(state, -3);
  return {};
}

} // namespace gisland
