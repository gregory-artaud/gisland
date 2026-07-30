#include "gisland/scene_template.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <variant>

namespace {

gisland::SceneTemplate text_template(gisland::TemplateValue<std::string> value,
                                     gisland::TemplateValue<std::string> role =
                                         std::string{"body"}) {
  return gisland::SceneTemplate{gisland::TemplateText{
      .value = std::move(value),
      .role = std::move(role),
      .truncation = std::string{"end"},
  }};
}

} // namespace

TEST_CASE("scene templates resolve literal and dotted bound values") {
  const auto literal = gisland::instantiate_template(text_template(std::string{"fixed"}), {});
  REQUIRE(literal.has_value());
  const auto *literal_text = std::get_if<gisland::Text>(&literal->value);
  REQUIRE(literal_text != nullptr);
  CHECK(literal_text->value == "fixed");

  const nlohmann::json snapshot{{"clock", {{"time", "14:35"}}}};
  const auto bound = gisland::instantiate_template(
      text_template(gisland::DataBinding{"clock.time"}), snapshot);
  REQUIRE(bound.has_value());
  const auto *bound_text = std::get_if<gisland::Text>(&bound->value);
  REQUIRE(bound_text != nullptr);
  CHECK(bound_text->value == "14:35");
}

TEST_CASE("scene template binding errors identify template and data paths") {
  const auto template_value = text_template(gisland::DataBinding{"time"});

  const auto missing = gisland::instantiate_template(template_value, nlohmann::json::object());
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().code == gisland::TemplateErrorCode::missing_data);
  CHECK(missing.error().template_path == "/value");
  CHECK(missing.error().data_path == "/time");

  const auto wrong_type = gisland::instantiate_template(template_value, {{"time", 1435}});
  REQUIRE_FALSE(wrong_type.has_value());
  CHECK(wrong_type.error().code == gisland::TemplateErrorCode::wrong_type);
  CHECK(wrong_type.error().template_path == "/value");
  CHECK(wrong_type.error().data_path == "/time");
}

TEST_CASE("scene templates instantiate every scalar primitive strictly") {
  const gisland::SceneTemplate icon{gisland::TemplateIcon{
      .name = gisland::DataBinding{"icon"},
      .accessible_label = std::string{"Clock"},
  }};
  const gisland::SceneTemplate spacer{gisland::TemplateSpacer{
      .flexible = gisland::DataBinding{"flexible"},
      .size_token = std::string{"small"},
  }};
  const gisland::SceneTemplate progress{gisland::TemplateProgress{
      .value = gisland::DataBinding{"progress"},
      .label = std::string{"Loading"},
      .state = std::string{"normal"},
  }};
  const nlohmann::json snapshot{{"icon", "clock"}, {"flexible", false}, {"progress", 0.5}};

  CHECK(gisland::instantiate_template(icon, snapshot).has_value());
  CHECK(gisland::instantiate_template(spacer, snapshot).has_value());
  CHECK(gisland::instantiate_template(progress, snapshot).has_value());

  const auto invalid_bool = gisland::instantiate_template(spacer, {{"flexible", "false"}});
  REQUIRE_FALSE(invalid_bool.has_value());
  CHECK(invalid_bool.error().code == gisland::TemplateErrorCode::wrong_type);

  const auto invalid_number = gisland::instantiate_template(progress, {{"progress", true}});
  REQUIRE_FALSE(invalid_number.has_value());
  CHECK(invalid_number.error().code == gisland::TemplateErrorCode::wrong_type);
}

TEST_CASE("instantiated templates retain scene validation bounds") {
  const auto invalid = gisland::instantiate_template(
      text_template(gisland::DataBinding{"value"}), {{"value", std::string(4097, 'x')}});
  REQUIRE_FALSE(invalid.has_value());
  CHECK(invalid.error().code == gisland::TemplateErrorCode::invalid_scene);
  CHECK(invalid.error().template_path == "/value");
}
