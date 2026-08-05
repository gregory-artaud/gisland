#include "gisland/scene.hpp"

#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace gisland {
namespace {

constexpr std::size_t maximum_depth = 16;
constexpr std::size_t maximum_nodes = 256;
constexpr std::size_t maximum_text_bytes = 4096;
constexpr std::size_t maximum_identifier_bytes = 128;

[[nodiscard]] SceneValidation validate_text(std::string_view value, const std::string &path) {
  if (value.size() > maximum_text_bytes) {
    return std::unexpected(SceneError{SceneErrorCode::text_too_long, path});
  }
  return {};
}

[[nodiscard]] SceneValidation validate_identifier(std::string_view value, const std::string &path) {
  if (value.size() > maximum_identifier_bytes) {
    return std::unexpected(SceneError{SceneErrorCode::identifier_too_long, path});
  }
  return {};
}

[[nodiscard]] std::vector<SceneChild> share_nodes(std::vector<SceneNode> nodes) {
  std::vector<SceneChild> children;
  children.reserve(nodes.size());
  for (auto &node : nodes) {
    children.push_back(std::make_shared<const SceneNode>(std::move(node)));
  }
  return children;
}

class SceneValidator {
public:
  [[nodiscard]] SceneValidation validate(const SceneNode &node, std::size_t depth,
                                         const std::string &path) {
    if (depth > maximum_depth) {
      return std::unexpected(SceneError{SceneErrorCode::too_deep, path});
    }
    ++node_count_;
    if (node_count_ > maximum_nodes) {
      return std::unexpected(SceneError{SceneErrorCode::too_many_nodes, path});
    }

    return std::visit(
        [this, depth, &path](const auto &primitive) {
          return this->validate_primitive(primitive, depth, path);
        },
        node.value);
  }

private:
  [[nodiscard]] static SceneValidation validate_primitive(const Text &text, std::size_t /*depth*/,
                                                          const std::string &path) {
    if (auto result = validate_text(text.value, path + "/value"); !result) {
      return result;
    }
    if (auto result = validate_identifier(text.role, path + "/role"); !result) {
      return result;
    }
    return validate_identifier(text.truncation, path + "/truncation");
  }

  [[nodiscard]] static SceneValidation validate_primitive(const Icon &icon, std::size_t /*depth*/,
                                                          const std::string &path) {
    if (auto result = validate_identifier(icon.name, path + "/name"); !result) {
      return result;
    }
    return validate_text(icon.accessible_label, path + "/accessible_label");
  }

  [[nodiscard]] static SceneValidation validate_primitive(const Image &image, std::size_t /*depth*/,
                                                          const std::string &path) {
    if (auto result = validate_identifier(image.resource_id, path + "/resource_id"); !result) {
      return result;
    }
    if (auto result = validate_identifier(image.role, path + "/role"); !result) {
      return result;
    }
    return validate_text(image.accessible_label, path + "/accessible_label");
  }

  [[nodiscard]] static SceneValidation
  validate_primitive(const Spacer &spacer, std::size_t /*depth*/, const std::string &path) {
    return validate_identifier(spacer.size_token, path + "/size_token");
  }

  [[nodiscard]] static SceneValidation
  validate_primitive(const Progress &progress, std::size_t /*depth*/, const std::string &path) {
    if (!std::isfinite(progress.value) || progress.value < 0.0 || progress.value > 1.0) {
      return std::unexpected(SceneError{SceneErrorCode::invalid_progress, path + "/value"});
    }
    if (auto result = validate_text(progress.label, path + "/label"); !result) {
      return result;
    }
    return validate_identifier(progress.state, path + "/state");
  }

  [[nodiscard]] SceneValidation validate_primitive(const Row &row, std::size_t depth,
                                                   const std::string &path) {
    if (auto result = validate_identifier(row.alignment, path + "/alignment"); !result) {
      return result;
    }
    if (auto result = validate_identifier(row.gap, path + "/gap"); !result) {
      return result;
    }
    return validate_children(row.children, depth, path);
  }

  [[nodiscard]] SceneValidation validate_primitive(const Column &column, std::size_t depth,
                                                   const std::string &path) {
    if (auto result = validate_identifier(column.alignment, path + "/alignment"); !result) {
      return result;
    }
    if (auto result = validate_identifier(column.gap, path + "/gap"); !result) {
      return result;
    }
    return validate_children(column.children, depth, path);
  }

  [[nodiscard]] SceneValidation validate_primitive(const Button &button, std::size_t depth,
                                                   const std::string &path) {
    if (button.action_id.empty()) {
      return std::unexpected(SceneError{SceneErrorCode::empty_action, path + "/action_id"});
    }
    if (auto result = validate_identifier(button.action_id, path + "/action_id"); !result) {
      return result;
    }
    if (auto result = validate_text(button.accessible_label, path + "/accessible_label"); !result) {
      return result;
    }
    return validate(*button.content, depth + 1, path + "/content");
  }

  [[nodiscard]] SceneValidation validate_children(const std::vector<SceneChild> &children,
                                                  std::size_t depth, const std::string &path) {
    for (std::size_t index = 0; index < children.size(); ++index) {
      auto result =
          validate(*children[index], depth + 1, path + "/children/" + std::to_string(index));
      if (!result.has_value()) {
        return result;
      }
    }
    return {};
  }

  std::size_t node_count_{0};
};

} // namespace

Row::Row(std::vector<SceneNode> nodes, std::string alignment_value, std::string gap_value)
    : children(share_nodes(std::move(nodes))), alignment(std::move(alignment_value)),
      gap(std::move(gap_value)) {}

Column::Column(std::vector<SceneNode> nodes, std::string alignment_value, std::string gap_value)
    : children(share_nodes(std::move(nodes))), alignment(std::move(alignment_value)),
      gap(std::move(gap_value)) {}

Button::Button(SceneNode content_value, std::string action_id_value, bool enabled_value,
               std::string accessible_label_value)
    : content(std::make_shared<const SceneNode>(std::move(content_value))),
      action_id(std::move(action_id_value)), enabled(enabled_value),
      accessible_label(std::move(accessible_label_value)) {}

SceneValidation validate_scene(const SceneNode &root) {
  return SceneValidator{}.validate(root, 1, "");
}

} // namespace gisland
