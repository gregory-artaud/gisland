#include "gisland/lua_scene.hpp"

#include "gisland/lua_value.hpp"

#include <lua.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace gisland {
namespace {

using Json = nlohmann::json;

constexpr std::size_t maximum_text_bytes = 4096;
constexpr std::size_t maximum_identifier_bytes = 128;
constexpr std::size_t maximum_resources = 16;
constexpr std::int64_t maximum_image_dimension = 512;
constexpr std::size_t maximum_decoded_resource_bytes = std::size_t{4} * 1024U * 1024U;
constexpr std::size_t maximum_base64_resource_bytes =
    ((maximum_decoded_resource_bytes + 2U) / 3U) * 4U;

[[nodiscard]] std::string path_error(std::string_view path, std::string_view message) {
  return std::string{path} + ": " + std::string{message};
}

[[nodiscard]] std::expected<void, std::string>
known_fields(const Json &object, const std::set<std::string_view> &fields, std::string_view path) {
  for (const auto &[key, value] : object.items()) {
    (void)value;
    if (!fields.contains(key)) {
      return std::unexpected(path_error(std::string{path} + '/' + key, "unknown field"));
    }
  }
  return {};
}

[[nodiscard]] std::expected<void, std::string>
required_string(const Json &object, std::string_view field, std::string_view path,
                std::size_t maximum = maximum_identifier_bytes, bool nonempty = false) {
  const auto iterator = object.find(field);
  const auto field_path = std::string{path} + '/' + std::string{field};
  if (iterator == object.end()) {
    return std::unexpected(path_error(field_path, "missing required field"));
  }
  if (!iterator->is_string()) {
    return std::unexpected(path_error(field_path, "expected a string"));
  }
  const auto &value = iterator->get_ref<const std::string &>();
  if (nonempty && value.empty()) {
    return std::unexpected(path_error(field_path, "must not be empty"));
  }
  if (value.size() > maximum) {
    return std::unexpected(path_error(field_path, "exceeds maximum byte count"));
  }
  return {};
}

[[nodiscard]] std::expected<void, std::string>
optional_string(const Json &object, std::string_view field, std::string_view path,
                std::size_t maximum = maximum_identifier_bytes) {
  if (!object.contains(field)) {
    return {};
  }
  return required_string(object, field, path, maximum);
}

[[nodiscard]] std::expected<void, std::string>
optional_bool(const Json &object, std::string_view field, std::string_view path) {
  const auto iterator = object.find(field);
  if (iterator != object.end() && !iterator->is_boolean()) {
    return std::unexpected(
        path_error(std::string{path} + '/' + std::string{field}, "expected a boolean"));
  }
  return {};
}

[[nodiscard]] std::expected<void, std::string> validate_scene(const Json &node,
                                                              std::string_view path);

[[nodiscard]] std::expected<void, std::string> validate_rich_content(const Json &content,
                                                                     std::string_view path) {
  if (!content.is_array()) {
    return std::unexpected(path_error(path, "expected an array"));
  }
  for (std::size_t index = 0; index < content.size(); ++index) {
    const auto item_path = std::string{path} + '/' + std::to_string(index);
    const auto &item = content[index];
    if (!item.is_object()) {
      return std::unexpected(path_error(item_path, "expected an object"));
    }
    if (auto result = required_string(item, "type", item_path); !result) {
      return result;
    }
    const auto &type = item.at("type").get_ref<const std::string &>();
    if (type == "text") {
      if (auto result = known_fields(item, {"type", "value", "emphasis"}, item_path); !result) {
        return result;
      }
      if (auto result = required_string(item, "value", item_path, maximum_text_bytes); !result) {
        return result;
      }
    } else if (type == "link") {
      if (auto result = known_fields(
              item, {"type", "value", "emphasis", "action_id", "accessible_label"}, item_path);
          !result) {
        return result;
      }
      if (auto result = required_string(item, "value", item_path, maximum_text_bytes); !result) {
        return result;
      }
      if (auto result =
              required_string(item, "action_id", item_path, maximum_identifier_bytes, true);
          !result) {
        return result;
      }
      if (auto result = required_string(item, "accessible_label", item_path, maximum_text_bytes);
          !result) {
        return result;
      }
    } else if (type == "inline_image") {
      if (auto result =
              known_fields(item, {"type", "resource_id", "role", "accessible_label"}, item_path);
          !result) {
        return result;
      }
      for (const auto field : {"resource_id", "role"}) {
        if (auto result = required_string(item, field, item_path); !result) {
          return result;
        }
      }
      if (auto result = required_string(item, "accessible_label", item_path, maximum_text_bytes);
          !result) {
        return result;
      }
    } else {
      return std::unexpected(path_error(item_path + "/type", "unknown rich content type"));
    }
    if (const auto emphasis = item.find("emphasis"); emphasis != item.end()) {
      if (!emphasis->is_array()) {
        return std::unexpected(path_error(item_path + "/emphasis", "expected an array"));
      }
      for (std::size_t emphasis_index = 0; emphasis_index < emphasis->size(); ++emphasis_index) {
        const auto emphasis_path = item_path + "/emphasis/" + std::to_string(emphasis_index);
        if (!(*emphasis)[emphasis_index].is_string()) {
          return std::unexpected(path_error(emphasis_path, "expected a string"));
        }
        const auto &value = (*emphasis)[emphasis_index].get_ref<const std::string &>();
        if (value != "bold" && value != "italic" && value != "underline") {
          return std::unexpected(path_error(emphasis_path, "unknown emphasis"));
        }
      }
    }
  }
  return {};
}

[[nodiscard]] std::expected<void, std::string> validate_scene(const Json &node,
                                                              std::string_view path) {
  if (!node.is_object()) {
    return std::unexpected(path_error(path, "expected a scene object"));
  }
  if (auto result = required_string(node, "type", path); !result) {
    return result;
  }
  const auto &type = node.at("type").get_ref<const std::string &>();
  if (type == "text") {
    if (auto result = known_fields(node, {"type", "value", "role", "truncation"}, path); !result) {
      return result;
    }
    if (auto result = required_string(node, "value", path, maximum_text_bytes); !result) {
      return result;
    }
    if (auto result = required_string(node, "role", path); !result) {
      return result;
    }
    return optional_string(node, "truncation", path);
  }
  if (type == "icon") {
    if (auto result = known_fields(node, {"type", "name", "accessible_label", "role"}, path);
        !result) {
      return result;
    }
    if (auto result = required_string(node, "name", path); !result) {
      return result;
    }
    if (auto result = required_string(node, "accessible_label", path, maximum_text_bytes);
        !result) {
      return result;
    }
    return optional_string(node, "role", path);
  }
  if (type == "image") {
    if (auto result = known_fields(node, {"type", "resource_id", "role", "accessible_label"}, path);
        !result) {
      return result;
    }
    for (const auto field : {"resource_id", "role"}) {
      if (auto result = required_string(node, field, path); !result) {
        return result;
      }
    }
    return required_string(node, "accessible_label", path, maximum_text_bytes);
  }
  if (type == "rich_text") {
    if (auto result = known_fields(node, {"type", "role", "content"}, path); !result) {
      return result;
    }
    if (auto result = required_string(node, "role", path); !result) {
      return result;
    }
    const auto content = node.find("content");
    if (content == node.end()) {
      return std::unexpected(path_error(std::string{path} + "/content", "missing required field"));
    }
    return validate_rich_content(*content, std::string{path} + "/content");
  }
  if (type == "row" || type == "column") {
    if (auto result = known_fields(node, {"type", "children", "alignment", "gap"}, path); !result) {
      return result;
    }
    const auto children = node.find("children");
    if (children == node.end() || !children->is_array()) {
      return std::unexpected(path_error(std::string{path} + "/children", "expected an array"));
    }
    if (auto result = optional_string(node, "alignment", path); !result) {
      return result;
    }
    if (auto result = optional_string(node, "gap", path); !result) {
      return result;
    }
    for (std::size_t index = 0; index < children->size(); ++index) {
      if (auto result = validate_scene((*children)[index],
                                       std::string{path} + "/children/" + std::to_string(index));
          !result) {
        return result;
      }
    }
    return {};
  }
  if (type == "spacer") {
    if (auto result = known_fields(node, {"type", "flexible", "size_token"}, path); !result) {
      return result;
    }
    if (auto result = optional_bool(node, "flexible", path); !result) {
      return result;
    }
    return optional_string(node, "size_token", path);
  }
  if (type == "progress") {
    if (auto result = known_fields(
            node, {"type", "value", "label", "state", "shape", "transition_from"}, path);
        !result) {
      return result;
    }
    const auto value = node.find("value");
    if (value == node.end() || !value->is_number()) {
      return std::unexpected(path_error(std::string{path} + "/value", "expected a number"));
    }
    const double progress = value->get<double>();
    if (!std::isfinite(progress) || progress < 0.0 || progress > 1.0) {
      return std::unexpected(path_error(std::string{path} + "/value", "must be between 0 and 1"));
    }
    if (const auto transition = node.find("transition_from"); transition != node.end()) {
      if (!transition->is_number()) {
        return std::unexpected(
            path_error(std::string{path} + "/transition_from", "expected a number"));
      }
      const double source = transition->get<double>();
      if (!std::isfinite(source) || source < 0.0 || source > 1.0) {
        return std::unexpected(
            path_error(std::string{path} + "/transition_from", "must be between 0 and 1"));
      }
    }
    if (auto result = optional_string(node, "label", path, maximum_text_bytes); !result) {
      return result;
    }
    for (const auto field : {"state", "shape"}) {
      if (auto result = optional_string(node, field, path); !result) {
        return result;
      }
    }
    if (node.contains("shape") && node.at("shape") != "linear" && node.at("shape") != "ring") {
      return std::unexpected(path_error(std::string{path} + "/shape", "unsupported shape"));
    }
    return {};
  }
  if (type == "indicator") {
    if (auto result = known_fields(node, {"type", "state", "accessible_label"}, path); !result) {
      return result;
    }
    if (auto result = required_string(node, "state", path); !result) {
      return result;
    }
    return required_string(node, "accessible_label", path, maximum_text_bytes);
  }
  if (type == "button" || type == "action_region") {
    if (auto result = known_fields(
            node, {"type", "content", "action_id", "enabled", "accessible_label"}, path);
        !result) {
      return result;
    }
    if (auto result = required_string(node, "action_id", path, maximum_identifier_bytes, true);
        !result) {
      return result;
    }
    if (auto result = required_string(node, "accessible_label", path, maximum_text_bytes);
        !result) {
      return result;
    }
    if (auto result = optional_bool(node, "enabled", path); !result) {
      return result;
    }
    const auto content = node.find("content");
    if (content == node.end()) {
      return std::unexpected(path_error(std::string{path} + "/content", "missing required field"));
    }
    return validate_scene(*content, std::string{path} + "/content");
  }
  return std::unexpected(path_error(std::string{path} + "/type", "unknown scene primitive"));
}

[[nodiscard]] std::expected<Json, std::string> lua_object(lua_State *state, int index,
                                                          std::string_view path) {
  auto converted = lua_value_to_json(state, index);
  if (!converted) {
    return std::unexpected(path_error(path, converted.error().message));
  }
  if (!converted->is_object()) {
    return std::unexpected(path_error(path, "expected a table object"));
  }
  return std::move(*converted);
}

[[nodiscard]] std::expected<Json, std::string> container_object(lua_State *state, int index,
                                                                std::string_view type) {
  if (!lua_istable(state, index)) {
    return std::unexpected(path_error("ui." + std::string{type}, "expected one table"));
  }
  const int table = lua_absindex(state, index);
  Json object = Json::object();
  Json positional = Json::array();
  std::size_t integer_keys = 0;
  std::size_t maximum_index = 0;
  lua_pushnil(state);
  while (lua_next(state, table) != 0) {
    if (lua_isinteger(state, -2)) {
      const auto key = lua_tointeger(state, -2);
      if (key < 1) {
        lua_pop(state, 2);
        return std::unexpected(
            path_error("ui." + std::string{type} + "/children", "child indexes must start at 1"));
      }
      ++integer_keys;
      maximum_index = std::max(maximum_index, static_cast<std::size_t>(key));
    } else if (lua_type(state, -2) == LUA_TSTRING) {
      std::size_t size = 0;
      const char *data = lua_tolstring(state, -2, &size);
      const std::string key{data, size};
      if (key != "alignment" && key != "gap" && key != "children") {
        lua_pop(state, 2);
        return std::unexpected(path_error("ui." + std::string{type} + '/' + key, "unknown field"));
      }
      auto value = lua_value_to_json(state, -1);
      if (!value) {
        lua_pop(state, 2);
        return std::unexpected(
            path_error("ui." + std::string{type} + '/' + key, value.error().message));
      }
      object[key] = std::move(*value);
    } else {
      lua_pop(state, 2);
      return std::unexpected(
          path_error("ui." + std::string{type}, "field keys must be strings or child indexes"));
    }
    lua_pop(state, 1);
  }
  if (integer_keys != 0 && object.contains("children")) {
    return std::unexpected(
        path_error("ui." + std::string{type} + "/children", "cannot mix positional children"));
  }
  if (integer_keys != maximum_index) {
    return std::unexpected(
        path_error("ui." + std::string{type} + "/children", "children must be contiguous"));
  }
  for (std::size_t child = 1; child <= maximum_index; ++child) {
    lua_geti(state, table, static_cast<lua_Integer>(child));
    auto value = lua_value_to_json(state, -1);
    lua_pop(state, 1);
    if (!value) {
      return std::unexpected(
          path_error("ui." + std::string{type} + "/children/" + std::to_string(child - 1),
                     value.error().message));
    }
    positional.push_back(std::move(*value));
  }
  if (!object.contains("children")) {
    object["children"] = std::move(positional);
  }
  object["type"] = type;
  return object;
}

[[nodiscard]] std::expected<void, std::string> validate_resource(const Json &resource,
                                                                 std::string_view path) {
  if (!resource.is_object()) {
    return std::unexpected(path_error(path, "expected an object"));
  }
  if (auto result = known_fields(resource, {"id", "format", "width", "height", "data"}, path);
      !result) {
    return result;
  }
  if (auto result = required_string(resource, "id", path, maximum_identifier_bytes, true);
      !result) {
    return result;
  }
  if (auto result = required_string(resource, "format", path); !result) {
    return result;
  }
  if (resource.at("format") != "rgba8") {
    return std::unexpected(path_error(std::string{path} + "/format", "unsupported format"));
  }
  for (const auto field : {"width", "height"}) {
    const auto iterator = resource.find(field);
    if (iterator == resource.end() || !iterator->is_number_integer()) {
      return std::unexpected(path_error(std::string{path} + '/' + field, "expected an integer"));
    }
    const auto value = iterator->get<std::int64_t>();
    if (value < 1 || value > maximum_image_dimension) {
      return std::unexpected(
          path_error(std::string{path} + '/' + field, "must be between 1 and 512"));
    }
  }
  return required_string(resource, "data", path, maximum_base64_resource_bytes);
}

[[nodiscard]] std::expected<void, std::string> validate_publication(const Json &context) {
  if (auto result = known_fields(
          context,
          {"context_id", "priority", "expires_in_ms", "views", "resources", "presentation"},
          "publish");
      !result) {
    return result;
  }
  if (auto result =
          required_string(context, "context_id", "publish", maximum_identifier_bytes, true);
      !result) {
    return result;
  }
  const auto priority = context.find("priority");
  if (priority == context.end() || !priority->is_number_integer()) {
    return std::unexpected(path_error("publish/priority", "expected an integer"));
  }
  const auto priority_value = priority->get<std::int64_t>();
  if (priority_value < std::numeric_limits<int>::min() ||
      priority_value > std::numeric_limits<int>::max()) {
    return std::unexpected(path_error("publish/priority", "integer is out of range"));
  }
  if (const auto expires = context.find("expires_in_ms");
      expires != context.end() &&
      (!expires->is_number_integer() || expires->get<std::int64_t>() < 0)) {
    return std::unexpected(path_error("publish/expires_in_ms", "expected a non-negative integer"));
  }
  const auto views = context.find("views");
  if (views == context.end() || !views->is_object()) {
    return std::unexpected(path_error("publish/views", "expected an object"));
  }
  if (auto result = known_fields(*views, {"compact", "expanded"}, "publish/views"); !result) {
    return result;
  }
  if (views->empty()) {
    return std::unexpected(path_error("publish/views", "at least one view is required"));
  }
  for (const auto field : {"compact", "expanded"}) {
    if (const auto view = views->find(field); view != views->end()) {
      if (auto result = validate_scene(*view, "publish/views/" + std::string{field}); !result) {
        return result;
      }
    }
  }
  if (const auto resources = context.find("resources"); resources != context.end()) {
    if (!resources->is_array()) {
      return std::unexpected(path_error("publish/resources", "expected an array"));
    }
    if (resources->size() > maximum_resources) {
      return std::unexpected(path_error("publish/resources", "too many resources"));
    }
    for (std::size_t index = 0; index < resources->size(); ++index) {
      if (auto result =
              validate_resource((*resources)[index], "publish/resources/" + std::to_string(index));
          !result) {
        return result;
      }
    }
  }
  if (const auto presentation = context.find("presentation"); presentation != context.end()) {
    if (!presentation->is_object()) {
      return std::unexpected(path_error("publish/presentation", "expected an object"));
    }
    if (auto result = known_fields(*presentation, {"reveal", "duration_ms", "compact_style"},
                                   "publish/presentation");
        !result) {
      return result;
    }
    for (const auto field : {"reveal", "compact_style"}) {
      if (auto result = optional_string(*presentation, field, "publish/presentation"); !result) {
        return result;
      }
    }
    if (const auto duration = presentation->find("duration_ms"); duration != presentation->end()) {
      if (!duration->is_number_integer()) {
        return std::unexpected(
            path_error("publish/presentation/duration_ms", "expected an integer"));
      }
      const auto milliseconds = duration->get<std::int64_t>();
      if (milliseconds < 1 || milliseconds > 60000) {
        return std::unexpected(
            path_error("publish/presentation/duration_ms", "must be between 1 and 60000"));
      }
    }
  }
  return {};
}

[[nodiscard]] LuaSceneApi *api(lua_State *state) {
  return static_cast<LuaSceneApi *>(lua_touserdata(state, lua_upvalueindex(1)));
}

} // namespace

LuaSceneApi::LuaSceneApi(Emit emit) : emit_(std::move(emit)) {}

std::expected<void, std::string> LuaSceneApi::register_into(lua_State *state) {
  lua_getglobal(state, "gisland");
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    return std::unexpected("gisland global must be registered before scene API");
  }
  for (const auto &[name, callback] : {
           std::pair{"publish", &LuaSceneApi::publish},
           std::pair{"dismiss", &LuaSceneApi::dismiss},
           std::pair{"log", &LuaSceneApi::log},
       }) {
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, callback, 1);
    lua_setfield(state, -2, name);
  }

  lua_createtable(state, 0, 11);
  for (const char *name : {"text", "icon", "image", "rich_text", "row", "column", "spacer",
                           "progress", "indicator", "button", "action_region"}) {
    lua_pushlightuserdata(state, this);
    lua_pushstring(state, name);
    lua_pushcclosure(state, &LuaSceneApi::construct, 2);
    lua_setfield(state, -2, name);
  }
  lua_setfield(state, -2, "ui");
  lua_pop(state, 1);
  return {};
}

int LuaSceneApi::publish(lua_State *state) {
  if (lua_gettop(state) != 1) {
    lua_pushliteral(state, "gisland.publish expects one context table");
    return lua_error(state);
  }
  bool failed = false;
  {
    auto context = lua_object(state, 1, "publish");
    if (!context) {
      api(state)->callback_error_ = context.error();
      failed = true;
    } else if (auto result = validate_publication(*context); !result) {
      api(state)->callback_error_ = result.error();
      failed = true;
    } else {
      context->emplace("type", "publish");
      auto emitted = api(state)->emit_(std::move(*context));
      if (!emitted) {
        api(state)->callback_error_ = emitted.error();
        failed = true;
      }
    }
  }
  if (failed) {
    const auto *scene_api = api(state);
    lua_pushlstring(state, scene_api->callback_error_.data(), scene_api->callback_error_.size());
    return lua_error(state);
  }
  return 0;
}

int LuaSceneApi::dismiss(lua_State *state) {
  std::size_t size = 0;
  const char *value = lua_tolstring(state, 1, &size);
  if (lua_gettop(state) != 1 || lua_type(state, 1) != LUA_TSTRING || size == 0 ||
      size > maximum_identifier_bytes) {
    lua_pushliteral(state, "dismiss/context_id: expected 1 to 128 string bytes");
    return lua_error(state);
  }
  bool failed = false;
  {
    auto emitted =
        api(state)->emit_(Json{{"type", "dismiss"}, {"context_id", std::string{value, size}}});
    if (!emitted) {
      api(state)->callback_error_ = emitted.error();
      failed = true;
    }
  }
  if (failed) {
    const auto *scene_api = api(state);
    lua_pushlstring(state, scene_api->callback_error_.data(), scene_api->callback_error_.size());
    return lua_error(state);
  }
  return 0;
}

int LuaSceneApi::log(lua_State *state) {
  if (lua_gettop(state) != 2 || lua_type(state, 1) != LUA_TSTRING ||
      lua_type(state, 2) != LUA_TSTRING) {
    lua_pushliteral(state, "gisland.log expects level and message strings");
    return lua_error(state);
  }
  std::size_t level_size = 0;
  std::size_t message_size = 0;
  const char *level = lua_tolstring(state, 1, &level_size);
  const char *message = lua_tolstring(state, 2, &message_size);
  if (message_size > maximum_text_bytes) {
    lua_pushliteral(state, "log/message: exceeds maximum byte count");
    return lua_error(state);
  }
  bool failed = false;
  {
    const std::string level_value{level, level_size};
    if (level_value != "debug" && level_value != "info" && level_value != "warning" &&
        level_value != "error") {
      api(state)->callback_error_ = "log/level: unknown log level";
      failed = true;
    } else {
      auto emitted = api(state)->emit_(Json{{"type", "log"},
                                            {"level", level_value},
                                            {"message", std::string{message, message_size}}});
      if (!emitted) {
        api(state)->callback_error_ = emitted.error();
        failed = true;
      }
    }
  }
  if (failed) {
    const auto *scene_api = api(state);
    lua_pushlstring(state, scene_api->callback_error_.data(), scene_api->callback_error_.size());
    return lua_error(state);
  }
  return 0;
}

int LuaSceneApi::construct(lua_State *state) {
  if (lua_gettop(state) != 1 || !lua_istable(state, 1)) {
    lua_pushliteral(state, "scene constructor expects one table");
    return lua_error(state);
  }
  bool failed = false;
  {
    std::size_t type_size = 0;
    const char *type_data = lua_tolstring(state, lua_upvalueindex(2), &type_size);
    const std::string type{type_data, type_size};
    std::expected<Json, std::string> object = type == "row" || type == "column"
                                                  ? container_object(state, 1, type)
                                                  : lua_object(state, 1, "ui." + type);
    if (!object) {
      api(state)->callback_error_ = object.error();
      failed = true;
    } else {
      object->emplace("type", type);
      if (auto result = validate_scene(*object, "ui." + type); !result) {
        api(state)->callback_error_ = result.error();
        failed = true;
      } else if (auto pushed = push_json_to_lua(state, *object); !pushed) {
        api(state)->callback_error_ = pushed.error().message;
        failed = true;
      }
    }
  }
  if (failed) {
    const auto *scene_api = api(state);
    lua_pushlstring(state, scene_api->callback_error_.data(), scene_api->callback_error_.size());
    return lua_error(state);
  }
  return 1;
}

} // namespace gisland
