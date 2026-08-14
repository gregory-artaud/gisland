#include "gisland/protocol.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace gisland {
namespace {

using Json = nlohmann::json;

constexpr std::size_t maximum_image_resources = 16;
constexpr std::uint32_t maximum_image_dimension = 512;
constexpr std::size_t maximum_image_bytes = std::size_t{4} * 1024U * 1024U;

[[nodiscard]] ProtocolError error_at(std::string path, std::string message) {
  return ProtocolError{std::move(path), std::move(message)};
}

// The two arguments intentionally share a type: one is the existing JSON pointer and the other
// is the statically selected child field.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] std::string field_path(std::string_view parent, std::string_view field) {
  std::string path{parent};
  path += '/';
  path += field;
  return path;
}

[[nodiscard]] std::expected<void, ProtocolError>
reject_unknown_fields(const Json &object, const std::set<std::string_view> &allowed,
                      std::string_view path) {
  for (const auto &[key, value] : object.items()) {
    (void)value;
    if (!allowed.contains(key)) {
      return std::unexpected(error_at(field_path(path, key), "unknown field"));
    }
  }
  return {};
}

[[nodiscard]] std::expected<const Json *, ProtocolError>
required_field(const Json &object, std::string_view key, std::string_view parent_path) {
  if (!object.is_object()) {
    return std::unexpected(error_at(std::string{parent_path}, "expected an object"));
  }

  const auto iterator = object.find(std::string{key});
  if (iterator == object.end()) {
    return std::unexpected(error_at(field_path(parent_path, key), "missing required field"));
  }
  return &*iterator;
}

[[nodiscard]] std::expected<std::string, ProtocolError>
required_string(const Json &object, std::string_view key, std::string_view parent_path) {
  auto field = required_field(object, key, parent_path);
  if (!field.has_value()) {
    return std::unexpected(field.error());
  }
  if (!(*field)->is_string()) {
    return std::unexpected(error_at(field_path(parent_path, key), "expected a string"));
  }
  return (*field)->get<std::string>();
}

[[nodiscard]] std::expected<std::string, ProtocolError>
optional_string(const Json &object, std::string_view key, std::string default_value,
                std::string_view parent_path) {
  const auto iterator = object.find(std::string{key});
  if (iterator == object.end()) {
    return default_value;
  }
  if (!iterator->is_string()) {
    return std::unexpected(error_at(field_path(parent_path, key), "expected a string"));
  }
  return iterator->get<std::string>();
}

[[nodiscard]] std::expected<std::optional<std::uint64_t>, ProtocolError>
optional_invocation_id(const Json &object) {
  const auto iterator = object.find("invocation_id");
  if (iterator == object.end()) {
    return std::nullopt;
  }
  if (!iterator->is_string()) {
    return std::unexpected(error_at("/invocation_id", "expected a decimal string"));
  }
  const auto &text = iterator->get_ref<const std::string &>();
  if (text.empty() || text.size() > 20) {
    return std::unexpected(error_at("/invocation_id", "invalid invocation ID"));
  }
  std::uint64_t value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return std::unexpected(error_at("/invocation_id", "invalid invocation ID"));
  }
  return std::optional{value};
}

template <typename Integer>
[[nodiscard]] std::expected<Integer, ProtocolError>
required_integer(const Json &object, std::string_view key, std::string_view parent_path) {
  auto field = required_field(object, key, parent_path);
  if (!field.has_value()) {
    return std::unexpected(field.error());
  }
  if (!(*field)->is_number_integer() && !(*field)->is_number_unsigned()) {
    return std::unexpected(error_at(field_path(parent_path, key), "expected an integer"));
  }
  try {
    return (*field)->get<Integer>();
  } catch (const Json::exception &) {
    return std::unexpected(error_at(field_path(parent_path, key), "integer is out of range"));
  }
}

[[nodiscard]] std::expected<double, ProtocolError>
required_number(const Json &object, std::string_view key, std::string_view parent_path) {
  auto field = required_field(object, key, parent_path);
  if (!field.has_value()) {
    return std::unexpected(field.error());
  }
  if (!(*field)->is_number()) {
    return std::unexpected(error_at(field_path(parent_path, key), "expected a number"));
  }
  try {
    return (*field)->get<double>();
  } catch (const Json::exception &) {
    return std::unexpected(error_at(field_path(parent_path, key), "number is out of range"));
  }
}

[[nodiscard]] std::expected<bool, ProtocolError> optional_bool(const Json &object,
                                                               std::string_view key,
                                                               bool default_value,
                                                               std::string_view parent_path) {
  const auto iterator = object.find(std::string{key});
  if (iterator == object.end()) {
    return default_value;
  }
  if (!iterator->is_boolean()) {
    return std::unexpected(error_at(field_path(parent_path, key), "expected a boolean"));
  }
  return iterator->get<bool>();
}

[[nodiscard]] std::expected<bool, ProtocolError>
required_bool(const Json &object, std::string_view key, std::string_view parent_path) {
  auto field = required_field(object, key, parent_path);
  if (!field.has_value()) {
    return std::unexpected(field.error());
  }
  if (!(*field)->is_boolean()) {
    return std::unexpected(error_at(field_path(parent_path, key), "expected a boolean"));
  }
  return (*field)->get<bool>();
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_scene(const Json &object,
                                                                  const std::string &path);

[[nodiscard]] std::expected<std::vector<SceneNode>, ProtocolError>
parse_children(const Json &object, const std::string &path) {
  auto field = required_field(object, "children", path);
  if (!field.has_value()) {
    return std::unexpected(field.error());
  }
  if (!(*field)->is_array()) {
    return std::unexpected(error_at(path + "/children", "expected an array"));
  }

  std::vector<SceneNode> children;
  children.reserve((*field)->size());
  for (std::size_t index = 0; index < (*field)->size(); ++index) {
    auto child = parse_scene((*field)->at(index), path + "/children/" + std::to_string(index));
    if (!child.has_value()) {
      return std::unexpected(child.error());
    }
    children.push_back(std::move(*child));
  }
  return children;
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_text(const Json &object,
                                                                 const std::string &path) {
  auto value = required_string(object, "value", path);
  auto role = required_string(object, "role", path);
  auto truncation = optional_string(object, "truncation", "end", path);
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  if (!role.has_value()) {
    return std::unexpected(role.error());
  }
  if (!truncation.has_value()) {
    return std::unexpected(truncation.error());
  }
  return SceneNode{Text{std::move(*value), std::move(*role), std::move(*truncation)}};
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_icon(const Json &object,
                                                                 const std::string &path) {
  auto name = required_string(object, "name", path);
  auto accessible_label = required_string(object, "accessible_label", path);
  auto role = optional_string(object, "role", "body", path);
  if (!name.has_value()) {
    return std::unexpected(name.error());
  }
  if (!accessible_label.has_value()) {
    return std::unexpected(accessible_label.error());
  }
  if (!role.has_value()) {
    return std::unexpected(role.error());
  }
  return SceneNode{Icon{std::move(*name), std::move(*accessible_label), std::move(*role)}};
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_image(const Json &object,
                                                                  const std::string &path) {
  auto resource_id = required_string(object, "resource_id", path);
  auto role = required_string(object, "role", path);
  auto accessible_label = required_string(object, "accessible_label", path);
  if (!resource_id) {
    return std::unexpected(resource_id.error());
  }
  if (!role) {
    return std::unexpected(role.error());
  }
  if (!accessible_label) {
    return std::unexpected(accessible_label.error());
  }
  return SceneNode{Image{std::move(*resource_id), std::move(*role), std::move(*accessible_label)}};
}

[[nodiscard]] std::expected<std::vector<TextEmphasis>, ProtocolError>
parse_emphasis(const Json &object, const std::string &path) {
  const auto iterator = object.find("emphasis");
  if (iterator == object.end()) {
    return std::vector<TextEmphasis>{};
  }
  if (!iterator->is_array()) {
    return std::unexpected(error_at(path + "/emphasis", "expected an array"));
  }
  std::vector<TextEmphasis> emphasis;
  emphasis.reserve(iterator->size());
  for (std::size_t index = 0; index < iterator->size(); ++index) {
    const auto &entry = iterator->at(index);
    if (!entry.is_string()) {
      return std::unexpected(
          error_at(path + "/emphasis/" + std::to_string(index), "expected a string"));
    }
    const auto value = entry.get<std::string>();
    if (value == "bold") {
      emphasis.push_back(TextEmphasis::bold);
    } else if (value == "italic") {
      emphasis.push_back(TextEmphasis::italic);
    } else if (value == "underline") {
      emphasis.push_back(TextEmphasis::underline);
    } else {
      return std::unexpected(
          error_at(path + "/emphasis/" + std::to_string(index), "unknown text emphasis"));
    }
  }
  return emphasis;
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_rich_text(const Json &object,
                                                                      const std::string &path) {
  auto role = required_string(object, "role", path);
  auto content_field = required_field(object, "content", path);
  if (!role) {
    return std::unexpected(role.error());
  }
  if (!content_field) {
    return std::unexpected(content_field.error());
  }
  if (!(*content_field)->is_array()) {
    return std::unexpected(error_at(path + "/content", "expected an array"));
  }
  std::vector<RichContent> content;
  content.reserve((*content_field)->size());
  for (std::size_t index = 0; index < (*content_field)->size(); ++index) {
    const auto &entry = (*content_field)->at(index);
    const std::string item_path = path + "/content/" + std::to_string(index);
    if (!entry.is_object()) {
      return std::unexpected(error_at(item_path, "expected a rich content object"));
    }
    auto type = required_string(entry, "type", item_path);
    if (!type) {
      return std::unexpected(type.error());
    }
    if (*type == "text" || *type == "link") {
      auto value = required_string(entry, "value", item_path);
      auto emphasis = parse_emphasis(entry, item_path);
      if (!value) {
        return std::unexpected(value.error());
      }
      if (!emphasis) {
        return std::unexpected(emphasis.error());
      }
      if (*type == "text") {
        content.emplace_back(RichTextSpan{std::move(*value), std::move(*emphasis)});
        continue;
      }
      auto action_id = required_string(entry, "action_id", item_path);
      auto label = required_string(entry, "accessible_label", item_path);
      if (!action_id) {
        return std::unexpected(action_id.error());
      }
      if (!label) {
        return std::unexpected(label.error());
      }
      content.emplace_back(RichLinkSpan{std::move(*value), std::move(*emphasis),
                                        std::move(*action_id), std::move(*label)});
      continue;
    }
    if (*type == "inline_image") {
      auto resource_id = required_string(entry, "resource_id", item_path);
      auto image_role = required_string(entry, "role", item_path);
      auto label = required_string(entry, "accessible_label", item_path);
      if (!resource_id) {
        return std::unexpected(resource_id.error());
      }
      if (!image_role) {
        return std::unexpected(image_role.error());
      }
      if (!label) {
        return std::unexpected(label.error());
      }
      content.emplace_back(
          RichInlineImage{std::move(*resource_id), std::move(*image_role), std::move(*label)});
      continue;
    }
    return std::unexpected(error_at(item_path + "/type", "unknown rich content item"));
  }
  return SceneNode{RichText{std::move(*role), std::move(content)}};
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_spacer(const Json &object,
                                                                   const std::string &path) {
  auto flexible = optional_bool(object, "flexible", true, path);
  auto size_token = optional_string(object, "size_token", "", path);
  if (!flexible.has_value()) {
    return std::unexpected(flexible.error());
  }
  if (!size_token.has_value()) {
    return std::unexpected(size_token.error());
  }
  return SceneNode{Spacer{*flexible, std::move(*size_token)}};
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_progress(const Json &object,
                                                                     const std::string &path) {
  auto value = required_number(object, "value", path);
  auto label = optional_string(object, "label", "", path);
  auto state = optional_string(object, "state", "normal", path);
  auto shape = optional_string(object, "shape", "linear", path);
  std::optional<double> transition_from;
  if (const auto iterator = object.find("transition_from"); iterator != object.end()) {
    if (!iterator->is_number()) {
      return std::unexpected(error_at(path + "/transition_from", "expected a number"));
    }
    transition_from = iterator->get<double>();
  }
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  if (!label.has_value()) {
    return std::unexpected(label.error());
  }
  if (!state.has_value()) {
    return std::unexpected(state.error());
  }
  if (!shape.has_value()) {
    return std::unexpected(shape.error());
  }
  ProgressShape progress_shape{};
  if (*shape == "linear") {
    progress_shape = ProgressShape::linear;
  } else if (*shape == "ring") {
    progress_shape = ProgressShape::ring;
  } else {
    return std::unexpected(error_at(path + "/shape", "unsupported progress shape"));
  }
  return SceneNode{
      Progress{*value, std::move(*label), std::move(*state), progress_shape, transition_from}};
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_indicator(const Json &object,
                                                                      const std::string &path) {
  auto state = required_string(object, "state", path);
  auto label = required_string(object, "accessible_label", path);
  if (!state) {
    return std::unexpected(state.error());
  }
  if (!label) {
    return std::unexpected(label.error());
  }
  std::vector<IndicatorEffect> effects;
  if (const auto iterator = object.find("effects"); iterator != object.end()) {
    if (!iterator->is_array()) {
      return std::unexpected(error_at(path + "/effects", "expected an array"));
    }
    std::set<std::string> unique;
    effects.reserve(iterator->size());
    for (std::size_t index = 0; index < iterator->size(); ++index) {
      const auto item_path = path + "/effects/" + std::to_string(index);
      const auto &entry = iterator->at(index);
      if (!entry.is_string()) {
        return std::unexpected(error_at(item_path, "expected a string"));
      }
      const auto value = entry.get<std::string>();
      if (!unique.insert(value).second) {
        return std::unexpected(error_at(item_path, "indicator effect must be unique"));
      }
      if (value == "shadow") {
        effects.push_back(IndicatorEffect::shadow);
      } else if (value == "glow") {
        effects.push_back(IndicatorEffect::glow);
      } else if (value == "breathe") {
        effects.push_back(IndicatorEffect::breathe);
      } else {
        return std::unexpected(error_at(item_path, "unknown indicator effect"));
      }
    }
  }
  return SceneNode{Indicator{std::move(*state), std::move(*label), std::move(effects)}};
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_row(const Json &object,
                                                                const std::string &path) {
  auto children = parse_children(object, path);
  auto alignment = optional_string(object, "alignment", "center", path);
  auto gap = optional_string(object, "gap", "normal", path);
  if (!children.has_value()) {
    return std::unexpected(children.error());
  }
  if (!alignment.has_value()) {
    return std::unexpected(alignment.error());
  }
  if (!gap.has_value()) {
    return std::unexpected(gap.error());
  }
  return SceneNode{Row{std::move(*children), std::move(*alignment), std::move(*gap)}};
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_column(const Json &object,
                                                                   const std::string &path) {
  auto children = parse_children(object, path);
  auto alignment = optional_string(object, "alignment", "center", path);
  auto gap = optional_string(object, "gap", "normal", path);
  if (!children.has_value()) {
    return std::unexpected(children.error());
  }
  if (!alignment.has_value()) {
    return std::unexpected(alignment.error());
  }
  if (!gap.has_value()) {
    return std::unexpected(gap.error());
  }
  return SceneNode{Column{std::move(*children), std::move(*alignment), std::move(*gap)}};
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_button(const Json &object,
                                                                   const std::string &path) {
  auto content_field = required_field(object, "content", path);
  if (!content_field.has_value()) {
    return std::unexpected(content_field.error());
  }
  auto content = parse_scene(**content_field, path + "/content");
  auto action_id = required_string(object, "action_id", path);
  auto enabled = optional_bool(object, "enabled", true, path);
  auto accessible_label = required_string(object, "accessible_label", path);
  if (!content.has_value()) {
    return std::unexpected(content.error());
  }
  if (!action_id.has_value()) {
    return std::unexpected(action_id.error());
  }
  if (!enabled.has_value()) {
    return std::unexpected(enabled.error());
  }
  if (!accessible_label.has_value()) {
    return std::unexpected(accessible_label.error());
  }
  return SceneNode{
      Button{std::move(*content), std::move(*action_id), *enabled, std::move(*accessible_label)}};
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_action_region(const Json &object,
                                                                          const std::string &path) {
  auto content_field = required_field(object, "content", path);
  if (!content_field) {
    return std::unexpected(content_field.error());
  }
  auto content = parse_scene(**content_field, path + "/content");
  auto action_id = required_string(object, "action_id", path);
  auto enabled = optional_bool(object, "enabled", true, path);
  auto label = required_string(object, "accessible_label", path);
  if (!content) {
    return std::unexpected(content.error());
  }
  if (!action_id) {
    return std::unexpected(action_id.error());
  }
  if (!enabled) {
    return std::unexpected(enabled.error());
  }
  if (!label) {
    return std::unexpected(label.error());
  }
  return SceneNode{
      ActionRegion{std::move(*content), std::move(*action_id), *enabled, std::move(*label)}};
}

[[nodiscard]] std::expected<SceneNode, ProtocolError> parse_scene(const Json &object,
                                                                  const std::string &path) {
  if (!object.is_object()) {
    return std::unexpected(error_at(path, "expected a scene object"));
  }

  auto type = required_string(object, "type", path);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  if (*type == "text") {
    return parse_text(object, path);
  }
  if (*type == "icon") {
    return parse_icon(object, path);
  }
  if (*type == "image") {
    return parse_image(object, path);
  }
  if (*type == "rich_text") {
    return parse_rich_text(object, path);
  }
  if (*type == "row") {
    return parse_row(object, path);
  }
  if (*type == "column") {
    return parse_column(object, path);
  }
  if (*type == "spacer") {
    return parse_spacer(object, path);
  }
  if (*type == "progress") {
    return parse_progress(object, path);
  }
  if (*type == "indicator") {
    return parse_indicator(object, path);
  }
  if (*type == "button") {
    return parse_button(object, path);
  }
  if (*type == "action_region") {
    return parse_action_region(object, path);
  }
  return std::unexpected(error_at(path + "/type", "unknown scene primitive"));
}

[[nodiscard]] std::string scene_error_message(SceneErrorCode code) {
  switch (code) {
  case SceneErrorCode::too_deep:
    return "scene exceeds maximum depth";
  case SceneErrorCode::too_many_nodes:
    return "scene exceeds maximum node count";
  case SceneErrorCode::text_too_long:
    return "text exceeds maximum byte count";
  case SceneErrorCode::identifier_too_long:
    return "scene identifier exceeds maximum byte count";
  case SceneErrorCode::invalid_progress:
    return "progress value must be between 0 and 1";
  case SceneErrorCode::empty_action:
    return "action ID must not be empty";
  case SceneErrorCode::invalid_emphasis:
    return "text emphasis values must be unique";
  }
  return "invalid scene";
}

[[nodiscard]] std::optional<ProtocolError> scene_validation_error(const SceneNode &scene,
                                                                  const std::string &path) {
  const auto validation = validate_scene(scene);
  if (validation.has_value()) {
    return std::nullopt;
  }
  return error_at(path + validation.error().path, scene_error_message(validation.error().code));
}

[[nodiscard]] int base64_value(char value) {
  if (value >= 'A' && value <= 'Z') {
    return value - 'A';
  }
  if (value >= 'a' && value <= 'z') {
    return value - 'a' + 26;
  }
  if (value >= '0' && value <= '9') {
    return value - '0' + 52;
  }
  if (value == '+') {
    return 62;
  }
  if (value == '/') {
    return 63;
  }
  return -1;
}

[[nodiscard]] std::expected<std::vector<std::uint8_t>, ProtocolError>
decode_base64(std::string_view encoded, std::size_t expected_size, const std::string &path) {
  if (encoded.size() % 4U != 0) {
    return std::unexpected(error_at(path, "invalid base64 length"));
  }
  std::size_t padding = 0;
  if (!encoded.empty() && encoded.back() == '=') {
    padding = 1;
    if (encoded.size() >= 2 && encoded[encoded.size() - 2] == '=') {
      padding = 2;
    }
  }
  const std::size_t decoded_size = (encoded.size() / 4U) * 3U - padding;
  if (decoded_size != expected_size) {
    return std::unexpected(error_at(path, "decoded image byte count does not match dimensions"));
  }

  std::vector<std::uint8_t> decoded;
  decoded.reserve(decoded_size);
  for (std::size_t offset = 0; offset < encoded.size(); offset += 4) {
    const bool final = offset + 4 == encoded.size();
    const bool third_padding = encoded[offset + 2] == '=';
    const bool fourth_padding = encoded[offset + 3] == '=';
    if ((!final && (third_padding || fourth_padding)) || (third_padding && !fourth_padding)) {
      return std::unexpected(error_at(path, "invalid base64 padding"));
    }
    const int first = base64_value(encoded[offset]);
    const int second = base64_value(encoded[offset + 1]);
    const int third = third_padding ? 0 : base64_value(encoded[offset + 2]);
    const int fourth = fourth_padding ? 0 : base64_value(encoded[offset + 3]);
    if (first < 0 || second < 0 || third < 0 || fourth < 0) {
      return std::unexpected(error_at(path, "invalid base64 character"));
    }
    if ((third_padding && (second & 0x0F) != 0) ||
        (fourth_padding && !third_padding && (third & 0x03) != 0)) {
      return std::unexpected(error_at(path, "non-canonical base64 padding bits"));
    }
    decoded.push_back(static_cast<std::uint8_t>((first << 2) | (second >> 4)));
    if (!third_padding) {
      decoded.push_back(static_cast<std::uint8_t>((second << 4) | (third >> 2)));
    }
    if (!fourth_padding) {
      decoded.push_back(static_cast<std::uint8_t>((third << 6) | fourth));
    }
  }
  return decoded;
}

[[nodiscard]] std::expected<std::vector<ImageResource>, ProtocolError>
parse_image_resources(const Json &object) {
  const auto iterator = object.find("resources");
  if (iterator == object.end()) {
    return std::vector<ImageResource>{};
  }
  if (!iterator->is_array()) {
    return std::unexpected(error_at("/resources", "expected an array"));
  }
  if (iterator->size() > maximum_image_resources) {
    return std::unexpected(error_at("/resources", "too many image resources"));
  }

  std::vector<ImageResource> resources;
  resources.reserve(iterator->size());
  std::set<std::string> identifiers;
  std::size_t total_bytes = 0;
  for (std::size_t index = 0; index < iterator->size(); ++index) {
    const auto &resource = iterator->at(index);
    const std::string path = "/resources/" + std::to_string(index);
    auto id = required_string(resource, "id", path);
    auto format = required_string(resource, "format", path);
    auto width = required_integer<std::int64_t>(resource, "width", path);
    auto height = required_integer<std::int64_t>(resource, "height", path);
    auto data = required_string(resource, "data", path);
    if (!id) {
      return std::unexpected(id.error());
    }
    if (id->empty() || id->size() > 128) {
      return std::unexpected(error_at(path + "/id", "resource ID must contain 1 to 128 bytes"));
    }
    if (!identifiers.insert(*id).second) {
      return std::unexpected(error_at(path + "/id", "resource ID must be unique"));
    }
    if (!format) {
      return std::unexpected(format.error());
    }
    if (*format != "rgba8") {
      return std::unexpected(error_at(path + "/format", "unsupported image format"));
    }
    if (!width) {
      return std::unexpected(width.error());
    }
    if (*width < 1 || *width > maximum_image_dimension) {
      return std::unexpected(error_at(path + "/width", "image width must be between 1 and 512"));
    }
    if (!height) {
      return std::unexpected(height.error());
    }
    if (*height < 1 || *height > maximum_image_dimension) {
      return std::unexpected(error_at(path + "/height", "image height must be between 1 and 512"));
    }
    if (!data) {
      return std::unexpected(data.error());
    }
    const auto byte_count =
        static_cast<std::size_t>(*width) * static_cast<std::size_t>(*height) * 4U;
    if (byte_count > maximum_image_bytes - total_bytes) {
      return std::unexpected(error_at("/resources", "decoded image resources exceed 4 MiB"));
    }
    auto pixels = decode_base64(*data, byte_count, path + "/data");
    if (!pixels) {
      return std::unexpected(pixels.error());
    }
    total_bytes += byte_count;
    resources.push_back(
        ImageResource{std::move(*id), ImageFormat::rgba8, static_cast<std::uint32_t>(*width),
                      static_cast<std::uint32_t>(*height),
                      std::make_shared<const std::vector<std::uint8_t>>(std::move(*pixels))});
  }
  return resources;
}

[[nodiscard]] std::optional<ProtocolError>
validate_image_references(const SceneNode &scene, const std::set<std::string> &resources,
                          const std::string &path) {
  return std::visit(
      [&resources, &path](const auto &primitive) -> std::optional<ProtocolError> {
        using Primitive = std::decay_t<decltype(primitive)>;
        if constexpr (std::is_same_v<Primitive, Image>) {
          if (!resources.contains(primitive.resource_id)) {
            return error_at(path + "/resource_id", "image resource is not in this publication");
          }
        } else if constexpr (std::is_same_v<Primitive, RichText>) {
          for (std::size_t index = 0; index < primitive.content.size(); ++index) {
            if (const auto *image = std::get_if<RichInlineImage>(&primitive.content[index]);
                image != nullptr && !resources.contains(image->resource_id)) {
              return error_at(path + "/content/" + std::to_string(index) + "/resource_id",
                              "image resource is not in this publication");
            }
          }
        } else if constexpr (std::is_same_v<Primitive, Row> || std::is_same_v<Primitive, Column>) {
          for (std::size_t index = 0; index < primitive.children.size(); ++index) {
            if (auto result =
                    validate_image_references(*primitive.children[index], resources,
                                              path + "/children/" + std::to_string(index));
                result) {
              return result;
            }
          }
        } else if constexpr (std::is_same_v<Primitive, Button> ||
                             std::is_same_v<Primitive, ActionRegion>) {
          return validate_image_references(*primitive.content, resources, path + "/content");
        }
        return std::nullopt;
      },
      scene.value);
}

[[nodiscard]] std::expected<ModuleMessage, ProtocolError> parse_ready(const Json &object) {
  auto protocol_major = required_integer<int>(object, "protocol_major", "");
  auto protocol_minor = required_integer<int>(object, "protocol_minor", "");
  if (!protocol_major.has_value()) {
    return std::unexpected(protocol_major.error());
  }
  if (!protocol_minor.has_value()) {
    return std::unexpected(protocol_minor.error());
  }
  if (*protocol_major < 0) {
    return std::unexpected(error_at("/protocol_major", "protocol version must be non-negative"));
  }
  if (*protocol_minor < 0) {
    return std::unexpected(error_at("/protocol_minor", "protocol version must be non-negative"));
  }

  std::vector<std::string> capabilities;
  const auto capabilities_iterator = object.find("capabilities");
  if (capabilities_iterator != object.end()) {
    if (!capabilities_iterator->is_array()) {
      return std::unexpected(error_at("/capabilities", "expected an array"));
    }
    std::set<std::string> unique_capabilities;
    capabilities.reserve(capabilities_iterator->size());
    for (std::size_t index = 0; index < capabilities_iterator->size(); ++index) {
      const auto &entry = capabilities_iterator->at(index);
      const auto path = "/capabilities/" + std::to_string(index);
      if (!entry.is_string()) {
        return std::unexpected(error_at(path, "expected a string"));
      }
      auto capability = entry.get<std::string>();
      if (capability.empty()) {
        return std::unexpected(error_at(path, "capability must not be empty"));
      }
      if (!unique_capabilities.insert(capability).second) {
        return std::unexpected(error_at(path, "capability must be unique"));
      }
      capabilities.push_back(std::move(capability));
    }
  }
  return ModuleMessage{ReadyMessage{*protocol_major, *protocol_minor, std::move(capabilities)}};
}

[[nodiscard]] std::expected<ModuleMessage, ProtocolError> parse_dismiss(const Json &object) {
  auto context_id = required_string(object, "context_id", "");
  if (!context_id.has_value()) {
    return std::unexpected(context_id.error());
  }
  if (context_id->empty()) {
    return std::unexpected(error_at("/context_id", "context ID must not be empty"));
  }
  return ModuleMessage{DismissMessage{std::move(*context_id)}};
}

[[nodiscard]] std::expected<ModuleMessage, ProtocolError> parse_data(const Json &object) {
  auto value = required_field(object, "value", "");
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  if (!(*value)->is_object()) {
    return std::unexpected(error_at("/value", "expected an object"));
  }
  return ModuleMessage{DataMessage{**value}};
}

[[nodiscard]] std::expected<ModuleMessage, ProtocolError> parse_publish(const Json &object) {
  auto context_id = required_string(object, "context_id", "");
  auto priority = required_integer<int>(object, "priority", "");
  if (!context_id.has_value()) {
    return std::unexpected(context_id.error());
  }
  if (context_id->empty()) {
    return std::unexpected(error_at("/context_id", "context ID must not be empty"));
  }
  if (!priority.has_value()) {
    return std::unexpected(priority.error());
  }
  auto resources = parse_image_resources(object);
  if (!resources) {
    return std::unexpected(resources.error());
  }

  std::optional<std::chrono::milliseconds> expires_in;
  if (object.contains("expires_in_ms")) {
    auto milliseconds = required_integer<std::int64_t>(object, "expires_in_ms", "");
    if (!milliseconds.has_value()) {
      return std::unexpected(milliseconds.error());
    }
    if (*milliseconds < 0) {
      return std::unexpected(error_at("/expires_in_ms", "expiration must be non-negative"));
    }
    expires_in = std::chrono::milliseconds{*milliseconds};
  }

  std::optional<SceneNode> compact;
  std::optional<SceneNode> expanded;

  const auto views_iterator = object.find("views");
  if (views_iterator != object.end()) {
    if (!views_iterator->is_object()) {
      return std::unexpected(error_at("/views", "expected an object"));
    }
    if (object.contains("compact") || object.contains("expanded")) {
      return std::unexpected(
          error_at("/views", "cannot be combined with legacy compact or expanded fields"));
    }
    auto known_views = reject_unknown_fields(
        *views_iterator, std::set<std::string_view>{"compact", "expanded"}, "/views");
    if (!known_views) {
      return std::unexpected(known_views.error());
    }
    if (!views_iterator->contains("compact") && !views_iterator->contains("expanded")) {
      return std::unexpected(error_at("/views", "at least one view is required"));
    }
    if (const auto iterator = views_iterator->find("compact"); iterator != views_iterator->end()) {
      auto parsed = parse_scene(*iterator, "/views/compact");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      if (auto error = scene_validation_error(*parsed, "/views/compact"); error) {
        return std::unexpected(std::move(*error));
      }
      compact = std::move(*parsed);
    }
    if (const auto iterator = views_iterator->find("expanded"); iterator != views_iterator->end()) {
      auto parsed = parse_scene(*iterator, "/views/expanded");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      if (auto error = scene_validation_error(*parsed, "/views/expanded"); error) {
        return std::unexpected(std::move(*error));
      }
      expanded = std::move(*parsed);
    }
  } else {
    auto compact_field = required_field(object, "compact", "");
    if (!compact_field) {
      return std::unexpected(compact_field.error());
    }
    auto parsed_compact = parse_scene(**compact_field, "/compact");
    if (!parsed_compact) {
      return std::unexpected(parsed_compact.error());
    }
    if (auto error = scene_validation_error(*parsed_compact, "/compact"); error) {
      return std::unexpected(std::move(*error));
    }
    compact = std::move(*parsed_compact);

    const auto expanded_iterator = object.find("expanded");
    if (expanded_iterator != object.end() && !expanded_iterator->is_null()) {
      auto parsed_expanded = parse_scene(*expanded_iterator, "/expanded");
      if (!parsed_expanded) {
        return std::unexpected(parsed_expanded.error());
      }
      if (auto error = scene_validation_error(*parsed_expanded, "/expanded"); error) {
        return std::unexpected(std::move(*error));
      }
      expanded = std::move(*parsed_expanded);
    }
  }

  std::optional<PresentationIntent> presentation;
  const auto presentation_iterator = object.find("presentation");
  if (presentation_iterator != object.end()) {
    if (views_iterator == object.end()) {
      return std::unexpected(error_at("/presentation", "presentation requires independent views"));
    }
    if (!presentation_iterator->is_object()) {
      return std::unexpected(error_at("/presentation", "expected an object"));
    }
    auto known_presentation = reject_unknown_fields(
        *presentation_iterator,
        std::set<std::string_view>{"reveal", "duration_ms", "compact_style"}, "/presentation");
    if (!known_presentation) {
      return std::unexpected(known_presentation.error());
    }
    std::optional<Reveal> reveal;
    std::optional<std::chrono::milliseconds> duration;
    if (presentation_iterator->contains("reveal")) {
      auto reveal_name = required_string(*presentation_iterator, "reveal", "/presentation");
      if (!reveal_name) {
        return std::unexpected(reveal_name.error());
      }
      if (*reveal_name != "expanded") {
        return std::unexpected(error_at("/presentation/reveal", "unknown reveal value"));
      }
      if (!expanded) {
        return std::unexpected(error_at("/presentation", "presentation requires an expanded view"));
      }
      reveal = Reveal::expanded;
    }
    if (presentation_iterator->contains("duration_ms")) {
      if (!reveal) {
        return std::unexpected(
            error_at("/presentation/duration_ms", "duration requires expanded reveal"));
      }
      auto milliseconds =
          required_integer<std::int64_t>(*presentation_iterator, "duration_ms", "/presentation");
      if (!milliseconds) {
        return std::unexpected(milliseconds.error());
      }
      if (*milliseconds <= 0 || *milliseconds > 60000) {
        return std::unexpected(error_at("/presentation/duration_ms",
                                        "duration must be between 1 and 60000 milliseconds"));
      }
      duration = std::chrono::milliseconds{*milliseconds};
    }
    std::optional<std::string> compact_style;
    if (presentation_iterator->contains("compact_style")) {
      auto style = required_string(*presentation_iterator, "compact_style", "/presentation");
      if (!style) {
        return std::unexpected(style.error());
      }
      compact_style = std::move(*style);
    }
    if (!reveal && !compact_style) {
      return std::unexpected(error_at("/presentation", "presentation must not be empty"));
    }
    presentation = PresentationIntent{reveal, duration, std::move(compact_style)};
  }

  std::set<std::string> resource_ids;
  for (const auto &resource : *resources) {
    resource_ids.insert(resource.id);
  }
  const std::string path_prefix = views_iterator != object.end() ? "/views" : "";
  if (compact) {
    if (auto reference_error =
            validate_image_references(*compact, resource_ids, path_prefix + "/compact")) {
      return std::unexpected(std::move(*reference_error));
    }
  }
  if (expanded) {
    if (auto reference_error =
            validate_image_references(*expanded, resource_ids, path_prefix + "/expanded")) {
      return std::unexpected(std::move(*reference_error));
    }
  }

  return ModuleMessage{PublishMessage{
      std::move(*context_id), *priority, expires_in, std::move(compact), std::move(expanded),
      std::move(*resources), presentation, views_iterator != object.end()}};
}

[[nodiscard]] std::expected<ModuleMessage, ProtocolError> parse_action_result(const Json &object) {
  auto action_id = required_string(object, "action_id", "");
  auto accepted = required_bool(object, "accepted", "");
  auto invocation_id = optional_invocation_id(object);
  if (!action_id.has_value()) {
    return std::unexpected(action_id.error());
  }
  if (action_id->empty()) {
    return std::unexpected(error_at("/action_id", "action ID must not be empty"));
  }
  if (!accepted.has_value()) {
    return std::unexpected(accepted.error());
  }
  if (!invocation_id.has_value()) {
    return std::unexpected(invocation_id.error());
  }

  std::optional<std::string> message;
  const auto iterator = object.find("message");
  if (iterator != object.end() && !iterator->is_null()) {
    if (!iterator->is_string()) {
      return std::unexpected(error_at("/message", "expected a string"));
    }
    message = iterator->get<std::string>();
  }
  return ModuleMessage{
      ActionResultMessage{std::move(*action_id), *accepted, std::move(message), *invocation_id}};
}

[[nodiscard]] std::expected<ModuleMessage, ProtocolError> parse_log(const Json &object) {
  auto level = required_string(object, "level", "");
  auto message = required_string(object, "message", "");
  if (!level.has_value()) {
    return std::unexpected(level.error());
  }
  if (!message.has_value()) {
    return std::unexpected(message.error());
  }
  if (message->size() > 4096) {
    return std::unexpected(error_at("/message", "log message exceeds maximum byte count"));
  }

  LogLevel typed_level;
  if (*level == "debug") {
    typed_level = LogLevel::debug;
  } else if (*level == "info") {
    typed_level = LogLevel::info;
  } else if (*level == "warning") {
    typed_level = LogLevel::warning;
  } else if (*level == "error") {
    typed_level = LogLevel::error;
  } else {
    return std::unexpected(error_at("/level", "unknown log level"));
  }
  return ModuleMessage{LogMessage{typed_level, std::move(*message)}};
}

} // namespace

std::expected<ModuleMessage, ProtocolError> parse_module_message(std::string_view line) {
  Json object;
  try {
    object = Json::parse(line);
  } catch (const Json::parse_error &) {
    return std::unexpected(error_at("", "invalid JSON"));
  }

  if (!object.is_object()) {
    return std::unexpected(error_at("", "message must be a JSON object"));
  }

  auto type = required_string(object, "type", "");
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  if (*type == "ready") {
    return parse_ready(object);
  }
  if (*type == "publish") {
    return parse_publish(object);
  }
  if (*type == "dismiss") {
    return parse_dismiss(object);
  }
  if (*type == "action_result") {
    return parse_action_result(object);
  }
  if (*type == "log") {
    return parse_log(object);
  }
  if (*type == "data") {
    return parse_data(object);
  }
  return std::unexpected(error_at("/type", "unknown message type"));
}

std::string serialize_core_message(const CoreMessage &message) {
  const auto object = std::visit(
      [](const auto &typed_message) -> Json {
        using Message = std::remove_cvref_t<decltype(typed_message)>;
        if constexpr (std::is_same_v<Message, InitMessage>) {
          return Json{
              {"type", "init"},
              {"protocol",
               {{"minimum",
                 {{"major", typed_message.minimum.major}, {"minor", typed_message.minimum.minor}}},
                {"maximum",
                 {{"major", typed_message.maximum.major},
                  {"minor", typed_message.maximum.minor}}}}},
              {"instance_id", typed_message.instance_id},
              {"capabilities", typed_message.capabilities},
              {"configuration", typed_message.configuration},
              {"locale", typed_message.locale},
              {"timezone", typed_message.timezone},
          };
        } else if constexpr (std::is_same_v<Message, ActionMessage>) {
          Json action{{"type", "action"}, {"action_id", typed_message.action_id}};
          if (typed_message.value.has_value()) {
            action["value"] = *typed_message.value;
          }
          if (typed_message.invocation_id.has_value()) {
            action["invocation_id"] = std::to_string(*typed_message.invocation_id);
          }
          return action;
        } else if constexpr (std::is_same_v<Message, VisibilityMessage>) {
          std::string_view value;
          switch (typed_message.value) {
          case Visibility::hidden:
            value = "hidden";
            break;
          case Visibility::compact_active:
            value = "compact-active";
            break;
          case Visibility::expanded_active:
            value = "expanded-active";
            break;
          }
          return Json{{"type", "visibility"}, {"visibility", value}};
        } else {
          static_assert(std::is_same_v<Message, ShutdownMessage>);
          return Json{
              {"type", "shutdown"},
              {"reason", typed_message.reason},
              {"deadline_ms", typed_message.deadline.count()},
          };
        }
      },
      message);
  return object.dump() + '\n';
}

} // namespace gisland
