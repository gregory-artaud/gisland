#include "gisland/scene_template.hpp"

#include <cmath>
#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace gisland {
namespace {

struct BindingContext {
  const nlohmann::json &snapshot;
  std::vector<std::pair<std::string, const nlohmann::json *>> scopes;
};

[[nodiscard]] std::string data_path(std::string_view binding) {
  std::string result;
  std::size_t start = 0;
  while (start <= binding.size()) {
    const auto end = binding.find('.', start);
    result += '/';
    result +=
        binding.substr(start, end == std::string_view::npos ? binding.size() - start : end - start);
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
      binding.path.contains("..")) {
    return std::unexpected(
        TemplateError{TemplateErrorCode::invalid_binding, template_path, data_path(binding.path)});
  }

  const nlohmann::json *current = &context.snapshot;
  std::size_t start = 0;
  std::string traversed_path;
  const auto first_end = binding.path.find('.');
  const auto first = binding.path.substr(0, first_end);
  for (std::size_t index = context.scopes.size(); index > 0; --index) {
    const auto &scope = context.scopes[index - 1];
    if (scope.first == first) {
      current = scope.second;
      traversed_path = "/" + first;
      if (first_end == std::string::npos) {
        return current;
      }
      start = first_end + 1;
      break;
    }
  }
  while (start < binding.path.size()) {
    const auto end = binding.path.find('.', start);
    const auto segment = binding.path.substr(
        start, end == std::string::npos ? binding.path.size() - start : end - start);
    if (!current->is_object()) {
      return std::unexpected(TemplateError{TemplateErrorCode::wrong_type, template_path,
                                           traversed_path.empty() ? "/" : traversed_path});
    }
    const auto iterator = current->find(segment);
    if (iterator == current->end()) {
      return std::unexpected(
          TemplateError{TemplateErrorCode::missing_data, template_path, data_path(binding.path)});
    }
    current = &*iterator;
    traversed_path += "/" + segment;
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return current;
}

template <typename T>
[[nodiscard]] std::expected<T, TemplateError> resolve_value(const TemplateValue<T> &value,
                                                            const BindingContext &context,
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
      return std::unexpected(
          TemplateError{TemplateErrorCode::wrong_type, template_path, data_path(binding.path)});
    }
  } else if constexpr (std::is_same_v<T, bool>) {
    if (!json.is_boolean()) {
      return std::unexpected(
          TemplateError{TemplateErrorCode::wrong_type, template_path, data_path(binding.path)});
    }
  } else if constexpr (std::is_same_v<T, double>) {
    if (!json.is_number() || json.is_boolean()) {
      return std::unexpected(
          TemplateError{TemplateErrorCode::wrong_type, template_path, data_path(binding.path)});
    }
  }
  return json.template get<T>();
}

[[nodiscard]] TemplateError scene_error(const SceneError &error) {
  return TemplateError{TemplateErrorCode::invalid_scene, error.path, {}};
}

class Instantiator {
public:
  explicit Instantiator(const nlohmann::json &snapshot) : context_{snapshot, {}} {}

  [[nodiscard]] std::expected<SceneNode, TemplateError>
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  instantiate(const SceneTemplate &scene_template, const std::string &path = "") {
    ++node_count_;
    if (node_count_ > maximum_nodes) {
      return std::unexpected(TemplateError{TemplateErrorCode::invalid_scene, path, {}});
    }
    return std::visit(
        // The variant dispatch keeps each template primitive adjacent to its scene conversion.
        // NOLINTNEXTLINE(readability-function-cognitive-complexity)
        [this, &path](const auto &primitive) -> std::expected<SceneNode, TemplateError> {
          using Primitive = std::decay_t<decltype(primitive)>;
          if constexpr (std::is_same_v<Primitive, TemplateText>) {
            auto value = resolve_value(primitive.value, context_, path + "/value");
            auto role = resolve_value(primitive.role, context_, path + "/role");
            auto truncation = resolve_value(primitive.truncation, context_, path + "/truncation");
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
            auto name = resolve_value(primitive.name, context_, path + "/name");
            auto label =
                resolve_value(primitive.accessible_label, context_, path + "/accessible_label");
            if (!name) {
              return std::unexpected(name.error());
            }
            if (!label) {
              return std::unexpected(label.error());
            }
            return SceneNode{Icon{std::move(*name), std::move(*label)}};
          } else if constexpr (std::is_same_v<Primitive, TemplateSpacer>) {
            auto flexible = resolve_value(primitive.flexible, context_, path + "/flexible");
            auto size = resolve_value(primitive.size_token, context_, path + "/size_token");
            if (!flexible) {
              return std::unexpected(flexible.error());
            }
            if (!size) {
              return std::unexpected(size.error());
            }
            return SceneNode{Spacer{*flexible, std::move(*size)}};
          } else if constexpr (std::is_same_v<Primitive, TemplateProgress>) {
            auto value = resolve_value(primitive.value, context_, path + "/value");
            auto label = resolve_value(primitive.label, context_, path + "/label");
            auto state = resolve_value(primitive.state, context_, path + "/state");
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
          } else if constexpr (std::is_same_v<Primitive, TemplateRow>) {
            auto children = instantiate_children(primitive.children, path);
            auto alignment = resolve_value(primitive.alignment, context_, path + "/alignment");
            auto gap = resolve_value(primitive.gap, context_, path + "/gap");
            if (!children) {
              return std::unexpected(children.error());
            }
            if (!alignment) {
              return std::unexpected(alignment.error());
            }
            if (!gap) {
              return std::unexpected(gap.error());
            }
            return SceneNode{Row{std::move(*children), std::move(*alignment), std::move(*gap)}};
          } else if constexpr (std::is_same_v<Primitive, TemplateColumn>) {
            auto children = instantiate_children(primitive.children, path);
            auto alignment = resolve_value(primitive.alignment, context_, path + "/alignment");
            auto gap = resolve_value(primitive.gap, context_, path + "/gap");
            if (!children) {
              return std::unexpected(children.error());
            }
            if (!alignment) {
              return std::unexpected(alignment.error());
            }
            if (!gap) {
              return std::unexpected(gap.error());
            }
            return SceneNode{Column{std::move(*children), std::move(*alignment), std::move(*gap)}};
          } else if constexpr (std::is_same_v<Primitive, TemplateButton>) {
            if (primitive.content == nullptr) {
              return std::unexpected(
                  TemplateError{TemplateErrorCode::invalid_template, path + "/content", {}});
            }
            auto content = instantiate(*primitive.content, path + "/content");
            auto enabled = resolve_value(primitive.enabled, context_, path + "/enabled");
            auto label =
                resolve_value(primitive.accessible_label, context_, path + "/accessible_label");
            if (!content) {
              return std::unexpected(content.error());
            }
            if (!enabled) {
              return std::unexpected(enabled.error());
            }
            if (!label) {
              return std::unexpected(label.error());
            }
            return SceneNode{
                Button{std::move(*content), primitive.action_id, *enabled, std::move(*label)}};
          } else {
            return std::unexpected(TemplateError{TemplateErrorCode::invalid_template, "", ""});
          }
        },
        scene_template.value);
  }

private:
  static constexpr std::size_t maximum_nodes = 256;

  [[nodiscard]] std::expected<std::vector<SceneNode>, TemplateError>
  instantiate_children(const std::vector<TemplateChild> &children, const std::string &path) {
    std::vector<SceneNode> result;
    for (std::size_t index = 0; index < children.size(); ++index) {
      const auto child_path = path + "/children/" + std::to_string(index);
      if (const auto *child = std::get_if<SceneTemplatePtr>(&children[index]); child != nullptr) {
        if (*child == nullptr) {
          return std::unexpected(
              TemplateError{TemplateErrorCode::invalid_template, child_path, {}});
        }
        auto node = instantiate(**child, child_path);
        if (!node) {
          return std::unexpected(node.error());
        }
        result.push_back(std::move(*node));
        continue;
      }

      const auto &repeat = std::get<TemplateRepeat>(children[index]);
      if (repeat.alias.empty() || repeat.body == nullptr) {
        return std::unexpected(TemplateError{TemplateErrorCode::invalid_template, child_path, {}});
      }
      auto source = resolve_binding(repeat.source, context_, child_path + "/repeat");
      if (!source) {
        return std::unexpected(source.error());
      }
      if (!(**source).is_array()) {
        return std::unexpected(TemplateError{TemplateErrorCode::repeat_source_mismatch,
                                             child_path + "/repeat",
                                             data_path(repeat.source.path)});
      }
      for (std::size_t item_index = 0; item_index < (**source).size(); ++item_index) {
        context_.scopes.emplace_back(repeat.alias, &(**source)[item_index]);
        auto node = instantiate(*repeat.body, child_path + "/items/" + std::to_string(item_index));
        context_.scopes.pop_back();
        if (!node) {
          return std::unexpected(node.error());
        }
        result.push_back(std::move(*node));
      }
    }
    return result;
  }

  BindingContext context_;
  std::size_t node_count_{0};
};

} // namespace

std::expected<SceneNode, TemplateError> instantiate_template(const SceneTemplate &scene_template,
                                                             const nlohmann::json &snapshot) {
  auto result = Instantiator{snapshot}.instantiate(scene_template);
  if (!result.has_value()) {
    return result;
  }
  const auto validation = validate_scene(*result);
  if (!validation.has_value()) {
    return std::unexpected(scene_error(validation.error()));
  }
  return result;
}

ModuleViewState::ModuleViewState(SceneTemplate compact, std::optional<SceneTemplate> expanded)
    : compact_template_(std::move(compact)), expanded_template_(std::move(expanded)) {}

std::expected<void, TemplateError> ModuleViewState::apply(nlohmann::json snapshot) {
  auto compact = instantiate_template(compact_template_, snapshot);
  if (!compact) {
    return std::unexpected(compact.error());
  }
  std::optional<SceneNode> expanded;
  if (expanded_template_.has_value()) {
    auto candidate = instantiate_template(*expanded_template_, snapshot);
    if (!candidate) {
      return std::unexpected(candidate.error());
    }
    expanded = std::move(*candidate);
  }
  snapshot_ = std::move(snapshot);
  views_ = InstantiatedViews{std::move(*compact), std::move(expanded)};
  return {};
}

const std::optional<nlohmann::json> &ModuleViewState::snapshot() const { return snapshot_; }

const std::optional<InstantiatedViews> &ModuleViewState::views() const { return views_; }

} // namespace gisland
