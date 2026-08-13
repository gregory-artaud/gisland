#pragma once

#include <nlohmann/json_fwd.hpp>

#include <expected>
#include <functional>
#include <string>

struct lua_State;

namespace gisland {

class LuaSceneApi {
public:
  using Emit = std::function<std::expected<void, std::string>(nlohmann::json)>;

  explicit LuaSceneApi(Emit emit);

  [[nodiscard]] std::expected<void, std::string> register_into(lua_State *state);

private:
  static int publish(lua_State *state);
  static int dismiss(lua_State *state);
  static int log(lua_State *state);
  static int construct(lua_State *state);

  Emit emit_;
  std::string callback_error_;
};

} // namespace gisland
