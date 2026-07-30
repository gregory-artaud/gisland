#include "gisland/scene_template.hpp"

#include <cmath>
#include <string_view>
#include <type_traits>
#include <utility>

namespace gisland {
namespace {

struct BindingContext {
  const nlohmann::json &snapshot;
};

[[nodiscard]] std::string data_path(std::string_view binding) {
  std::string result;
  std::size_t start = 0;
  while (start <= binding.size()) {
    const auto end = binding.find('.', start);
    result += '/';
    result += binding.substr(start, end == std::string_view::npos ? binding.size() - start
                                                                  : end - start);
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return result;
}

[[nodiscard]] std::expected<const nlohmann::json *, TemplateError>
resolve_binding(const DataBinding &binding, const BindingContext &context,
                const std::string &template_path) {
  if (binding.path.empty() || binding.path.front() == '.' || binding.path.back() == '.' ||
      binding.path.find("..") != std::string::npos) {
    return std::unexpected(
        TemplateError{TemplateErrorCode::invalid_binding, template_path, data_path(binding.path)});
  }

  const nlohmann::json *current = &context.snapshot;
  std::size_t start = 0;
  while (start < binding.path.size()) {
    const auto end = binding.path.find('.', start);
    const auto segment = binding.path.substr(start, end == std::string::npos
                                                        ? binding.path.size() - start
                                                        : end - start);
    if (!current->is_object()) {
      return std::unexpected(TemplateError{TemplateErrorCode::missing_data, template_path,
                                            data_path(binding.path)});
    }
    const auto iterator = current->find(segment);
    if (iterator == current->end()) {
      return std::unexpected(TemplateError{TemplateErrorCode::missing_data, template_path,
                                            data_path(binding.path)});
    }
    current = &*iterator;
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return current;
}

template <typename T>
[[nodiscard]] std::expected<T, TemplateError>
resolve_value(const TemplateValue<T> &value, const BindingContext &context,
              const std::string &template_path) {
  if (const auto *literal = std::get_if<T>(&value); literal != nullptr) {
    return *literal;
  }
  const auto &binding = std::get<DataBinding>(value);
  const auto resolved = resolve_binding(binding, context, template_path);
  if (!resolved.has_value()) {
    return std::unexpected(resolved.error());
  }
  const auto &json = **resolved;
  if constexpr (std::is_same_v<T, std::string>) {
    if (!json.is_string()) {
      return std::unexpected(TemplateError{TemplateErrorCode::wrong_type, template_path,
                                            data_path(binding.path)});
    }
  } else if constexpr (std::is_same_v<T, bool>) {
    if (!json.is_boolean()) {
      return std::unexpected(TemplateError{TemplateErrorCode::wrong_type, template_path,
                                            data_path(binding.path)});
    }
  } else if constexpr (std::is_same_v<T, double>) {
    if (!json.is_number() || json.is_boolean()) {
      return std::unexpected(TemplateError{TemplateErrorCode::wrong_type, template_path,
                                            data_path(binding.path)});
    }
  }
  return json.template get<T>();
}

[[nodiscard]] TemplateError scene_error(const SceneError &error) {
  return TemplateError{TemplateErrorCode::invalid_scene, error.path, {}};
}

[[nodiscard]] std::expected<SceneNode, TemplateError>
instantiate_leaf(const SceneTemplate &scene_template, const BindingContext &context) {
  return std::visit(
      [&context](const auto &primitive) -> std::expected<SceneNode, TemplateError> {
        using Primitive = std::decay_t<decltype(primitive)>;
        if constexpr (std::is_same_v<Primitive, TemplateText>) {
          auto value = resolve_value(primitive.value, context, "/value");
          auto role = resolve_value(primitive.role, context, "/role");
          auto truncation = resolve_value(primitive.truncation, context, "/truncation");
          if (!value) {
            return std::unexpected(value.error());
          }
          if (!role) {
            return std::unexpected(role.error());
          }
          if (!truncation) {
            return std::unexpected(truncation.error());
          }
          return SceneNode{Text{std::move(*value), std::move(*role), std::move(*truncation)}};
        } else if constexpr (std::is_same_v<Primitive, TemplateIcon>) {
          auto name = resolve_value(primitive.name, context, "/name");
          auto label = resolve_value(primitive.accessible_label, context, "/accessible_label");
          if (!name) {
            return std::unexpected(name.error());
          }
          if (!label) {
            return std::unexpected(label.error());
          }
          return SceneNode{Icon{std::move(*name), std::move(*label)}};
        } else if constexpr (std::is_same_v<Primitive, TemplateSpacer>) {
          auto flexible = resolve_value(primitive.flexible, context, "/flexible");
          auto size = resolve_value(primitive.size_token, context, "/size_token");
          if (!flexible) {
            return std::unexpected(flexible.error());
          }
          if (!size) {
            return std::unexpected(size.error());
          }
          return SceneNode{Spacer{*flexible, std::move(*size)}};
        } else if constexpr (std::is_same_v<Primitive, TemplateProgress>) {
          auto value = resolve_value(primitive.value, context, "/value");
          auto label = resolve_value(primitive.label, context, "/label");
          auto state = resolve_value(primitive.state, context, "/state");
          if (!value) {
            return std::unexpected(value.error());
          }
          if (!label) {
            return std::unexpected(label.error());
          }
          if (!state) {
            return std::unexpected(state.error());
          }
          return SceneNode{Progress{*value, std::move(*label), std::move(*state)}};
        } else {
          return std::unexpected(
              TemplateError{TemplateErrorCode::invalid_template, "", ""});
        }
      },
      scene_template.value);
}

} // namespace

std::expected<SceneNode, TemplateError>
instantiate_template(const SceneTemplate &scene_template, const nlohmann::json &snapshot) {
  auto result = instantiate_leaf(scene_template, BindingContext{snapshot});
  if (!result.has_value()) {
    return result;
  }
  const auto validation = validate_scene(*result);
  if (!validation.has_value()) {
    return std::unexpected(scene_error(validation.error()));
  }
  return result;
}

} // namespace gisland
