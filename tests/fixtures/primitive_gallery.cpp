#include "primitive_gallery.hpp"

#include <string>
#include <utility>

namespace gisland::test {
namespace {

[[nodiscard]] SceneNode text(std::string value, std::string role = "body") {
  return SceneNode{Text{std::move(value), std::move(role)}};
}

[[nodiscard]] SceneNode icon(std::string name, std::string label) {
  return SceneNode{Icon{std::move(name), std::move(label)}};
}

} // namespace

SceneNode primitive_gallery() {
  return SceneNode{Column{
      {text("Primitive gallery", "title"),
       SceneNode{Row{{icon("calendar", "Calendar"), text("Text and icon"),
                      SceneNode{Spacer{true, {}}}, text("Muted", "muted")},
                     "center",
                     "small"}},
       SceneNode{Progress{0.68, "Progress", "success"}},
       SceneNode{
           Row{{text("Indicators", "caption"), SceneNode{Indicator{"success", "Available"}},
                SceneNode{Indicator{"warning", "Delayed"}}, SceneNode{Indicator{"error", "Failed"}},
                SceneNode{Indicator{"muted", "Inactive"}}},
               "center",
               "small"}},
       SceneNode{Row{
           {SceneNode{Spacer{false, "small"}},
            SceneNode{Button{icon("chevron-left", "Previous"), "previous", true, "Previous item"}},
            SceneNode{Button{text("Disabled", "caption"), "disabled", false, "Disabled action"}},
            SceneNode{Button{icon("chevron-right", "Next"), "next", true, "Next item"}}},
           "center",
           "small"}}},
      "start",
      "normal"}};
}

} // namespace gisland::test
