#include "gisland/theme.hpp"

#include <toml++/toml.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace gisland {
namespace {

constexpr double maximum_pixels = 16384.0;
constexpr double maximum_font_size = 512.0;
constexpr double maximum_line_height = 8.0;
constexpr std::int64_t maximum_duration_ms = 60000;

[[nodiscard]] ThemeError error_at(std::string_view source_name, std::string path,
                                  std::string message, const toml::node *node = nullptr) {
  std::size_t line = 0;
  std::size_t column = 0;
  if (node != nullptr) {
    line = static_cast<std::size_t>(node->source().begin.line);
    column = static_cast<std::size_t>(node->source().begin.column);
  }
  return ThemeError{std::string{source_name}, std::move(path), std::move(message), line, column};
}

[[nodiscard]] std::expected<void, ThemeError>
reject_unknown(const toml::table &table, std::initializer_list<std::string_view> allowed,
               const std::string &path, std::string_view source_name) {
  const std::set<std::string_view> allowed_keys{allowed};
  for (const auto &[key, node] : table) {
    if (!allowed_keys.contains(key.str())) {
      std::string key_path = path;
      if (!key_path.empty()) {
        key_path.push_back('.');
      }
      key_path.append(key.str());
      return std::unexpected(error_at(source_name, std::move(key_path), "unknown key", &node));
    }
  }
  return {};
}

[[nodiscard]] std::expected<const toml::table *, ThemeError>
required_table(const toml::table &parent, std::string_view key, const std::string &path,
               std::string_view source_name) {
  const auto *node = parent.get(key);
  if (node == nullptr) {
    return std::unexpected(error_at(source_name, path, "missing required table"));
  }
  const auto *table = node->as_table();
  if (table == nullptr) {
    return std::unexpected(error_at(source_name, path, "expected a table", node));
  }
  return table;
}

[[nodiscard]] std::expected<std::string, ThemeError> required_string(const toml::table &table,
                                                                     std::string_view key,
                                                                     const std::string &path,
                                                                     std::string_view source_name) {
  const auto *node = table.get(key);
  if (node == nullptr) {
    return std::unexpected(error_at(source_name, path, "missing required string"));
  }
  const auto value = node->value_exact<std::string>();
  if (!value.has_value()) {
    return std::unexpected(error_at(source_name, path, "expected a string", node));
  }
  if (value->empty()) {
    return std::unexpected(error_at(source_name, path, "value must not be empty", node));
  }
  return *value;
}

[[nodiscard]] std::expected<double, ThemeError>
number_value(const toml::node *node, const std::string &path, std::string_view source_name,
             double minimum, double maximum, bool required = true, double default_value = 0.0) {
  if (node == nullptr) {
    if (!required) {
      return default_value;
    }
    return std::unexpected(error_at(source_name, path, "missing required number"));
  }

  double value = 0.0;
  if (const auto floating = node->value_exact<double>(); floating.has_value()) {
    value = *floating;
  } else if (const auto integer = node->value_exact<std::int64_t>(); integer.has_value()) {
    value = static_cast<double>(*integer);
  } else {
    return std::unexpected(error_at(source_name, path, "expected a number", node));
  }
  if (!std::isfinite(value) || value < minimum || value > maximum) {
    return std::unexpected(
        error_at(source_name, path, "number is outside the allowed range", node));
  }
  return value;
}

[[nodiscard]] int hex_digit(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

[[nodiscard]] std::expected<Rgba, ThemeError>
parse_color(const toml::node *node, const std::string &path, std::string_view source_name) {
  if (node == nullptr) {
    return std::unexpected(error_at(source_name, path, "missing required color"));
  }
  const auto color = node->value_exact<std::string>();
  if (!color.has_value() || (color->size() != 7 && color->size() != 9) || color->front() != '#') {
    return std::unexpected(error_at(source_name, path, "expected #RRGGBB or #RRGGBBAA", node));
  }

  std::array<std::uint8_t, 4> channels{0, 0, 0, 255};
  const std::size_t channel_count = color->size() == 9 ? 4 : 3;
  for (std::size_t index = 0; index < channel_count; ++index) {
    const auto high = hex_digit((*color)[1 + (index * 2)]);
    const auto low = hex_digit((*color)[2 + (index * 2)]);
    if (high < 0 || low < 0) {
      return std::unexpected(error_at(source_name, path, "expected #RRGGBB or #RRGGBBAA", node));
    }
    channels[index] = static_cast<std::uint8_t>((high * 16) + low);
  }
  return Rgba{channels[0], channels[1], channels[2], channels[3]};
}

[[nodiscard]] std::expected<Theme::Palette, ThemeError>
parse_palette(const toml::table &root, std::string_view source_name) {
  auto table = required_table(root, "palette", "palette", source_name);
  if (!table) {
    return std::unexpected(table.error());
  }
  constexpr std::array roles{"surface", "foreground", "muted", "accent",
                             "success", "warning",    "error"};
  auto keys = reject_unknown(
      **table, {"surface", "foreground", "muted", "accent", "success", "warning", "error"},
      "palette", source_name);
  if (!keys) {
    return std::unexpected(keys.error());
  }

  Theme::Palette palette;
  for (const std::string_view role : roles) {
    auto color = parse_color((*table)->get(role), "palette." + std::string{role}, source_name);
    if (!color) {
      return std::unexpected(color.error());
    }
    palette.emplace(role, *color);
  }
  return palette;
}

[[nodiscard]] std::expected<Theme::FontResources, ThemeError>
parse_fonts(const toml::table &root, std::string_view source_name) {
  auto table = required_table(root, "fonts", "fonts", source_name);
  if (!table) {
    return std::unexpected(table.error());
  }
  if ((*table)->empty()) {
    return std::unexpected(error_at(source_name, "fonts", "at least one font is required"));
  }

  Theme::FontResources fonts;
  for (const auto &[key, node] : **table) {
    const std::string id{key.str()};
    if (id.empty()) {
      return std::unexpected(
          error_at(source_name, "fonts", "semantic key must not be empty", &node));
    }
    const auto path = node.value_exact<std::string>();
    if (!path.has_value() || path->empty()) {
      return std::unexpected(
          error_at(source_name, "fonts." + id, "expected a non-empty path", &node));
    }
    fonts.emplace(id, *path);
  }
  return fonts;
}

[[nodiscard]] std::expected<Theme::Typography, ThemeError>
parse_typography(const toml::table &root, std::string_view source_name) {
  auto table = required_table(root, "typography", "typography", source_name);
  if (!table) {
    return std::unexpected(table.error());
  }
  if (!(*table)->contains("body")) {
    return std::unexpected(error_at(source_name, "typography.body", "missing required role"));
  }

  Theme::Typography typography;
  for (const auto &[key, node] : **table) {
    const std::string role{key.str()};
    if (role.empty()) {
      return std::unexpected(
          error_at(source_name, "typography", "semantic key must not be empty", &node));
    }
    const auto *role_table = node.as_table();
    if (role_table == nullptr) {
      return std::unexpected(
          error_at(source_name, "typography." + role, "expected a table", &node));
    }
    auto keys = reject_unknown(*role_table, {"font", "color", "size", "weight", "line_height"},
                               "typography." + role, source_name);
    if (!keys) {
      return std::unexpected(keys.error());
    }
    auto font = required_string(*role_table, "font", "typography." + role + ".font", source_name);
    std::string color_role{"foreground"};
    if (const auto *color = role_table->get("color"); color != nullptr) {
      const auto value = color->value_exact<std::string>();
      if (!value.has_value() || value->empty()) {
        return std::unexpected(error_at(source_name, "typography." + role + ".color",
                                        "expected a non-empty palette role", color));
      }
      color_role = *value;
    }
    auto size = number_value(role_table->get("size"), "typography." + role + ".size", source_name,
                             std::numeric_limits<double>::min(), maximum_font_size);
    auto weight = number_value(role_table->get("weight"), "typography." + role + ".weight",
                               source_name, 1.0, 1000.0, false, 400.0);
    auto line_height = number_value(
        role_table->get("line_height"), "typography." + role + ".line_height", source_name,
        std::numeric_limits<double>::min(), maximum_line_height, false, 1.2);
    if (!font) {
      return std::unexpected(font.error());
    }
    if (!size) {
      return std::unexpected(size.error());
    }
    if (!weight) {
      return std::unexpected(weight.error());
    }
    if (std::floor(*weight) != *weight) {
      return std::unexpected(error_at(source_name, "typography." + role + ".weight",
                                      "weight must be an integer", role_table->get("weight")));
    }
    if (!line_height) {
      return std::unexpected(line_height.error());
    }
    typography.emplace(role, TypographyRole{std::move(*font), std::move(color_role), *size,
                                            static_cast<std::uint16_t>(*weight), *line_height});
  }
  return typography;
}

[[nodiscard]] std::expected<Theme::PixelTokens, ThemeError>
parse_pixel_tokens(const toml::table &root, std::string_view table_name,
                   std::string_view source_name) {
  const std::string base_path{table_name};
  auto table = required_table(root, table_name, base_path, source_name);
  if (!table) {
    return std::unexpected(table.error());
  }
  if (!(*table)->contains("normal")) {
    return std::unexpected(error_at(source_name, base_path + ".normal", "missing required token"));
  }

  Theme::PixelTokens tokens;
  for (const auto &[key, node] : **table) {
    const std::string token{key.str()};
    if (token.empty()) {
      return std::unexpected(
          error_at(source_name, base_path, "semantic key must not be empty", &node));
    }
    std::string token_path = base_path;
    token_path.push_back('.');
    token_path.append(token);
    auto value = number_value(&node, token_path, source_name, 0.0, maximum_pixels);
    if (!value) {
      return std::unexpected(value.error());
    }
    tokens.emplace(token, *value);
  }
  return tokens;
}

[[nodiscard]] std::expected<ViewGeometry, ThemeError>
parse_geometry(const toml::table &view, std::string_view name, std::string_view source_name) {
  const std::string path = "view." + std::string{name};
  auto table = required_table(view, name, path, source_name);
  if (!table) {
    return std::unexpected(table.error());
  }
  auto keys = reject_unknown(**table,
                             {"padding", "padding_horizontal", "padding_vertical", "radius",
                              "border", "min_width", "max_width", "min_height", "max_height"},
                             path, source_name);
  if (!keys) {
    return std::unexpected(keys.error());
  }

  const auto parse = [&](std::string_view key, double minimum) {
    return number_value((*table)->get(key), path + "." + std::string{key}, source_name, minimum,
                        maximum_pixels);
  };
  const auto *uniform_padding = (*table)->get("padding");
  const auto *horizontal_padding = (*table)->get("padding_horizontal");
  const auto *vertical_padding = (*table)->get("padding_vertical");
  auto padding = [&]() -> std::expected<std::pair<double, double>, ThemeError> {
    if (uniform_padding != nullptr) {
      if (horizontal_padding != nullptr) {
        return std::unexpected(error_at(source_name, path + ".padding_horizontal",
                                        "cannot combine uniform and axis-specific padding",
                                        horizontal_padding));
      }
      if (vertical_padding != nullptr) {
        return std::unexpected(error_at(source_name, path + ".padding_vertical",
                                        "cannot combine uniform and axis-specific padding",
                                        vertical_padding));
      }
      auto value = parse("padding", 0.0);
      if (!value) {
        return std::unexpected(value.error());
      }
      return std::pair{*value, *value};
    }
    if (horizontal_padding == nullptr) {
      return std::unexpected(error_at(source_name, path + ".padding_horizontal",
                                      "missing required horizontal padding"));
    }
    if (vertical_padding == nullptr) {
      return std::unexpected(
          error_at(source_name, path + ".padding_vertical", "missing required vertical padding"));
    }
    auto horizontal = parse("padding_horizontal", 0.0);
    auto vertical = parse("padding_vertical", 0.0);
    if (!horizontal) {
      return std::unexpected(horizontal.error());
    }
    if (!vertical) {
      return std::unexpected(vertical.error());
    }
    return std::pair{*horizontal, *vertical};
  }();
  auto radius = parse("radius", 0.0);
  auto border = parse("border", 0.0);
  auto min_width = parse("min_width", std::numeric_limits<double>::min());
  auto max_width = parse("max_width", std::numeric_limits<double>::min());
  auto min_height = parse("min_height", std::numeric_limits<double>::min());
  auto max_height = parse("max_height", std::numeric_limits<double>::min());
  if (!padding) {
    return std::unexpected(padding.error());
  }
  for (const auto *result : {&radius, &border, &min_width, &max_width, &min_height, &max_height}) {
    if (!result->has_value()) {
      return std::unexpected(result->error());
    }
  }
  if (*max_width < *min_width) {
    return std::unexpected(error_at(source_name, path + ".max_width",
                                    "maximum width must not be smaller than minimum width",
                                    (*table)->get("max_width")));
  }
  if (*max_height < *min_height) {
    return std::unexpected(error_at(source_name, path + ".max_height",
                                    "maximum height must not be smaller than minimum height",
                                    (*table)->get("max_height")));
  }
  if ((padding->first * 2.0) >= *min_width) {
    const std::string padding_path =
        uniform_padding != nullptr ? ".padding" : ".padding_horizontal";
    return std::unexpected(
        error_at(source_name, path + padding_path,
                 "horizontal padding must leave positive minimum content width",
                 uniform_padding != nullptr ? uniform_padding : horizontal_padding));
  }
  if ((padding->second * 2.0) >= *min_height) {
    const std::string padding_path = uniform_padding != nullptr ? ".padding" : ".padding_vertical";
    return std::unexpected(
        error_at(source_name, path + padding_path,
                 "vertical padding must leave positive minimum content height",
                 uniform_padding != nullptr ? uniform_padding : vertical_padding));
  }
  return ViewGeometry{padding->first, padding->second, *radius,     *border,
                      *min_width,     *max_width,      *min_height, *max_height};
}

[[nodiscard]] std::expected<ThemeViews, ThemeError> parse_views(const toml::table &root,
                                                                std::string_view source_name) {
  auto view = required_table(root, "view", "view", source_name);
  if (!view) {
    return std::unexpected(view.error());
  }
  auto keys = reject_unknown(**view, {"compact", "expanded"}, "view", source_name);
  if (!keys) {
    return std::unexpected(keys.error());
  }
  auto compact = parse_geometry(**view, "compact", source_name);
  auto expanded = parse_geometry(**view, "expanded", source_name);
  if (!compact) {
    return std::unexpected(compact.error());
  }
  if (!expanded) {
    return std::unexpected(expanded.error());
  }
  return ThemeViews{*compact, *expanded};
}

[[nodiscard]] std::expected<ThemeColor, ThemeError>
parse_theme_color(const toml::node *node, const std::string &path, std::string_view source_name) {
  if (node == nullptr) {
    return std::unexpected(error_at(source_name, path, "missing required color"));
  }
  const auto value = node->value_exact<std::string>();
  if (!value.has_value() || value->empty()) {
    return std::unexpected(
        error_at(source_name, path, "expected a palette role or #RRGGBB[AA]", node));
  }
  if (value->front() != '#') {
    return ThemeColor{*value};
  }
  auto color = parse_color(node, path, source_name);
  if (!color) {
    return std::unexpected(color.error());
  }
  return ThemeColor{*color};
}

[[nodiscard]] std::expected<ShadowStyle, ThemeError> parse_shadow(const toml::table &root,
                                                                  std::string_view source_name) {
  auto table = required_table(root, "shadow", "shadow", source_name);
  if (!table) {
    return std::unexpected(table.error());
  }
  auto keys = reject_unknown(**table, {"offset_x", "offset_y", "blur", "spread", "color"}, "shadow",
                             source_name);
  if (!keys) {
    return std::unexpected(keys.error());
  }

  auto offset_x = number_value((*table)->get("offset_x"), "shadow.offset_x", source_name,
                               -maximum_pixels, maximum_pixels);
  auto offset_y = number_value((*table)->get("offset_y"), "shadow.offset_y", source_name,
                               -maximum_pixels, maximum_pixels);
  auto blur = number_value((*table)->get("blur"), "shadow.blur", source_name, 0.0, maximum_pixels);
  auto spread =
      number_value((*table)->get("spread"), "shadow.spread", source_name, 0.0, maximum_pixels);
  auto color = parse_theme_color((*table)->get("color"), "shadow.color", source_name);
  if (!offset_x) {
    return std::unexpected(offset_x.error());
  }
  if (!offset_y) {
    return std::unexpected(offset_y.error());
  }
  if (!blur) {
    return std::unexpected(blur.error());
  }
  if (!spread) {
    return std::unexpected(spread.error());
  }
  if (!color) {
    return std::unexpected(color.error());
  }
  return ShadowStyle{*offset_x, *offset_y, *blur, *spread, std::move(*color)};
}

[[nodiscard]] std::expected<std::chrono::milliseconds, ThemeError>
parse_duration(const toml::table &table, std::string_view key, const std::string &path,
               std::string_view source_name) {
  const auto *node = table.get(key);
  if (node == nullptr) {
    return std::unexpected(error_at(source_name, path, "missing required integer duration"));
  }
  const auto value = node->value_exact<std::int64_t>();
  if (!value.has_value()) {
    return std::unexpected(error_at(source_name, path, "expected an integer duration", node));
  }
  if (*value < 0 || *value > maximum_duration_ms) {
    return std::unexpected(
        error_at(source_name, path, "duration is outside the allowed range", node));
  }
  return std::chrono::milliseconds{*value};
}

[[nodiscard]] std::expected<Easing, ThemeError> parse_easing(const toml::table &table,
                                                             std::string_view source_name) {
  constexpr std::string_view path = "animation.easing";
  const auto *node = table.get("easing");
  if (node == nullptr) {
    return std::unexpected(error_at(source_name, std::string{path}, "missing required easing"));
  }
  const auto value = node->value_exact<std::string>();
  if (!value.has_value()) {
    return std::unexpected(
        error_at(source_name, std::string{path}, "expected an easing string", node));
  }
  if (*value == "linear") {
    return Easing::linear;
  }
  if (*value == "ease-in") {
    return Easing::ease_in;
  }
  if (*value == "ease-out") {
    return Easing::ease_out;
  }
  if (*value == "ease-in-out") {
    return Easing::ease_in_out;
  }
  return std::unexpected(error_at(source_name, std::string{path}, "unsupported easing", node));
}

[[nodiscard]] std::expected<AnimationStyle, ThemeError>
parse_animation(const toml::table &root, std::string_view source_name) {
  auto table = required_table(root, "animation", "animation", source_name);
  if (!table) {
    return std::unexpected(table.error());
  }
  auto keys = reject_unknown(
      **table, {"compact_to_expanded_ms", "context_change_ms", "easing", "reduced_motion"},
      "animation", source_name);
  if (!keys) {
    return std::unexpected(keys.error());
  }
  auto reduced = required_table(**table, "reduced_motion", "animation.reduced_motion", source_name);
  if (!reduced) {
    return std::unexpected(reduced.error());
  }
  auto reduced_keys = reject_unknown(**reduced, {"compact_to_expanded_ms", "context_change_ms"},
                                     "animation.reduced_motion", source_name);
  if (!reduced_keys) {
    return std::unexpected(reduced_keys.error());
  }

  auto compact = parse_duration(**table, "compact_to_expanded_ms",
                                "animation.compact_to_expanded_ms", source_name);
  auto context =
      parse_duration(**table, "context_change_ms", "animation.context_change_ms", source_name);
  auto easing = parse_easing(**table, source_name);
  auto reduced_compact =
      parse_duration(**reduced, "compact_to_expanded_ms",
                     "animation.reduced_motion.compact_to_expanded_ms", source_name);
  auto reduced_context = parse_duration(**reduced, "context_change_ms",
                                        "animation.reduced_motion.context_change_ms", source_name);
  if (!compact) {
    return std::unexpected(compact.error());
  }
  if (!context) {
    return std::unexpected(context.error());
  }
  if (!easing) {
    return std::unexpected(easing.error());
  }
  if (!reduced_compact) {
    return std::unexpected(reduced_compact.error());
  }
  if (!reduced_context) {
    return std::unexpected(reduced_context.error());
  }
  return AnimationStyle{*compact, *context, *easing, {*reduced_compact, *reduced_context}};
}

[[nodiscard]] bool valid_codepoint(std::int64_t value) {
  constexpr std::int64_t maximum = 0x10FFFF;
  constexpr std::int64_t surrogate_first = 0xD800;
  constexpr std::int64_t surrogate_last = 0xDFFF;
  return value >= 0 && value <= maximum && (value < surrogate_first || value > surrogate_last);
}

[[nodiscard]] std::expected<Theme::Icons, ThemeError> parse_icons(const toml::table &root,
                                                                  std::string_view source_name) {
  auto table = required_table(root, "icons", "icons", source_name);
  if (!table) {
    return std::unexpected(table.error());
  }

  Theme::Icons icons;
  for (const auto &[key, node] : **table) {
    const std::string name{key.str()};
    if (name.empty()) {
      return std::unexpected(
          error_at(source_name, "icons", "semantic key must not be empty", &node));
    }
    const auto *icon = node.as_table();
    if (icon == nullptr) {
      return std::unexpected(error_at(source_name, "icons." + name, "expected a table", &node));
    }
    auto keys = reject_unknown(*icon, {"font", "codepoint"}, "icons." + name, source_name);
    if (!keys) {
      return std::unexpected(keys.error());
    }
    auto font = required_string(*icon, "font", "icons." + name + ".font", source_name);
    const auto *codepoint_node = icon->get("codepoint");
    const auto codepoint = codepoint_node == nullptr ? std::optional<std::int64_t>{}
                                                     : codepoint_node->value_exact<std::int64_t>();
    if (!font) {
      return std::unexpected(font.error());
    }
    if (!codepoint.has_value() || !valid_codepoint(*codepoint)) {
      return std::unexpected(error_at(source_name, "icons." + name + ".codepoint",
                                      "expected a valid Unicode scalar value", codepoint_node));
    }
    icons.emplace(name, IconGlyph{std::move(*font), static_cast<char32_t>(*codepoint)});
  }
  return icons;
}

[[nodiscard]] std::expected<void, ThemeError>
validate_font_references(const Theme::Typography &typography, const Theme::Icons &icons,
                         const Theme::FontResources &fonts, const Theme::Palette &palette,
                         const ShadowStyle &shadow, std::string_view source_name) {
  for (const auto &[role, style] : typography) {
    if (!fonts.contains(style.font)) {
      return std::unexpected(error_at(source_name, "typography." + role + ".font",
                                      "referenced font resource does not exist"));
    }
    if (!palette.contains(style.color)) {
      return std::unexpected(error_at(source_name, "typography." + role + ".color",
                                      "referenced palette role does not exist"));
    }
  }
  for (const auto &[name, icon] : icons) {
    if (!fonts.contains(icon.font)) {
      return std::unexpected(error_at(source_name, "icons." + name + ".font",
                                      "referenced font resource does not exist"));
    }
  }
  if (const auto *role = std::get_if<std::string>(&shadow.color);
      role != nullptr && !palette.contains(*role)) {
    return std::unexpected(
        error_at(source_name, "shadow.color", "referenced palette role does not exist"));
  }
  return {};
}

} // namespace

Theme::Theme(Palette palette, Typography typography, PixelTokens gaps, PixelTokens spacers,
             ThemeViews views, ShadowStyle shadow, AnimationStyle animation, FontResources fonts,
             Icons icons)
    : palette_(std::move(palette)), typography_(std::move(typography)), gaps_(std::move(gaps)),
      spacers_(std::move(spacers)), views_(views), shadow_(std::move(shadow)),
      animation_(animation), fonts_(std::move(fonts)), icons_(std::move(icons)) {}

std::expected<Theme, ThemeError> parse_theme(std::string_view text, std::string_view source_name) {
  try {
    const auto root = toml::parse(text, source_name);
    auto keys = reject_unknown(root,
                               {"palette", "typography", "gaps", "spacers", "view", "shadow",
                                "animation", "fonts", "icons"},
                               "", source_name);
    if (!keys) {
      return std::unexpected(keys.error());
    }

    auto palette = parse_palette(root, source_name);
    auto fonts = parse_fonts(root, source_name);
    auto typography = parse_typography(root, source_name);
    auto gaps = parse_pixel_tokens(root, "gaps", source_name);
    auto spacers = parse_pixel_tokens(root, "spacers", source_name);
    auto views = parse_views(root, source_name);
    auto shadow = parse_shadow(root, source_name);
    auto animation = parse_animation(root, source_name);
    auto icons = parse_icons(root, source_name);
    if (!palette) {
      return std::unexpected(palette.error());
    }
    if (!fonts) {
      return std::unexpected(fonts.error());
    }
    if (!typography) {
      return std::unexpected(typography.error());
    }
    if (!gaps) {
      return std::unexpected(gaps.error());
    }
    if (!spacers) {
      return std::unexpected(spacers.error());
    }
    if (!views) {
      return std::unexpected(views.error());
    }
    if (!shadow) {
      return std::unexpected(shadow.error());
    }
    if (!animation) {
      return std::unexpected(animation.error());
    }
    if (!icons) {
      return std::unexpected(icons.error());
    }
    auto references =
        validate_font_references(*typography, *icons, *fonts, *palette, *shadow, source_name);
    if (!references) {
      return std::unexpected(references.error());
    }
    return Theme{std::move(*palette),
                 std::move(*typography),
                 std::move(*gaps),
                 std::move(*spacers),
                 *views,
                 std::move(*shadow),
                 *animation,
                 std::move(*fonts),
                 std::move(*icons)};
  } catch (const toml::parse_error &error) {
    return std::unexpected(ThemeError{
        std::string{source_name},
        "",
        std::string{error.description()},
        static_cast<std::size_t>(error.source().begin.line),
        static_cast<std::size_t>(error.source().begin.column),
    });
  }
}

} // namespace gisland
