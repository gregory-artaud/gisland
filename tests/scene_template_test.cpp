#include "gisland/scene_template.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <variant>

namespace {

gisland::SceneTemplate text_template(gisland::TemplateValue<std::string> value,
                                     gisland::TemplateValue<std::string> role = std::string{
                                         "body"}) {
  return gisland::SceneTemplate{gisland::TemplateText{
      .value = std::move(value),
      .role = std::move(role),
      .truncation = std::string{"end"},
  }};
}

gisland::SceneTemplatePtr shared(gisland::SceneTemplate value) {
  return std::make_shared<const gisland::SceneTemplate>(std::move(value));
}

} // namespace

TEST_CASE("scene templates resolve literal and dotted bound values") {
  const auto literal = gisland::instantiate_template(text_template(std::string{"fixed"}), {});
  REQUIRE(literal.has_value());
  const auto *literal_text = std::get_if<gisland::Text>(&literal->value);
  REQUIRE(literal_text != nullptr);
  CHECK(literal_text->value == "fixed");

  const nlohmann::json snapshot{{"clock", {{"time", "14:35"}}}};
  const auto bound =
      gisland::instantiate_template(text_template(gisland::DataBinding{"clock.time"}), snapshot);
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

  const auto wrong_container = gisland::instantiate_template(
      text_template(gisland::DataBinding{"clock.time"}), {{"clock", "invalid"}});
  REQUIRE_FALSE(wrong_container.has_value());
  CHECK(wrong_container.error().code == gisland::TemplateErrorCode::wrong_type);
  CHECK(wrong_container.error().data_path == "/clock");
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
      .shape = std::string{"ring"},
  }};
  const nlohmann::json snapshot{{"icon", "clock"}, {"flexible", false}, {"progress", 0.5}};

  CHECK(gisland::instantiate_template(icon, snapshot).has_value());
  CHECK(gisland::instantiate_template(spacer, snapshot).has_value());
  const auto instantiated_progress = gisland::instantiate_template(progress, snapshot);
  REQUIRE(instantiated_progress.has_value());
  CHECK(std::get<gisland::Progress>(instantiated_progress->value).shape ==
        gisland::ProgressShape::ring);

  const auto invalid_bool = gisland::instantiate_template(spacer, {{"flexible", "false"}});
  REQUIRE_FALSE(invalid_bool.has_value());
  CHECK(invalid_bool.error().code == gisland::TemplateErrorCode::wrong_type);

  const auto invalid_number = gisland::instantiate_template(progress, {{"progress", true}});
  REQUIRE_FALSE(invalid_number.has_value());
  CHECK(invalid_number.error().code == gisland::TemplateErrorCode::wrong_type);
}

TEST_CASE("instantiated templates retain scene validation bounds") {
  const auto invalid = gisland::instantiate_template(text_template(gisland::DataBinding{"value"}),
                                                     {{"value", std::string(4097, 'x')}});
  REQUIRE_FALSE(invalid.has_value());
  CHECK(invalid.error().code == gisland::TemplateErrorCode::invalid_scene);
  CHECK(invalid.error().template_path == "/value");
}

TEST_CASE("nested repeats expand arrays in deterministic source order") {
  const auto day =
      shared(text_template(gisland::DataBinding{"day.label"}, gisland::DataBinding{"day.role"}));
  const auto week = shared(gisland::SceneTemplate{gisland::TemplateRow{
      .children = {gisland::TemplateRepeat{gisland::DataBinding{"week"}, "day", day}},
  }});
  const gisland::SceneTemplate calendar{gisland::TemplateColumn{
      .children = {gisland::TemplateRepeat{gisland::DataBinding{"weeks"}, "week", week}},
  }};
  const nlohmann::json snapshot{
      {"weeks",
       {{{{"label", "29"}, {"role", "muted"}}, {{"label", "30"}, {"role", "today"}}},
        {{{"label", "1"}, {"role", "body"}}}}}};

  const auto result = gisland::instantiate_template(calendar, snapshot);

  REQUIRE(result.has_value());
  const auto *column = std::get_if<gisland::Column>(&result->value);
  REQUIRE(column != nullptr);
  REQUIRE(column->children.size() == 2);
  const auto *first_week = std::get_if<gisland::Row>(&column->children[0]->value);
  REQUIRE(first_week != nullptr);
  REQUIRE(first_week->children.size() == 2);
  const auto *first_day = std::get_if<gisland::Text>(&first_week->children[0]->value);
  const auto *second_day = std::get_if<gisland::Text>(&first_week->children[1]->value);
  REQUIRE(first_day != nullptr);
  REQUIRE(second_day != nullptr);
  CHECK(first_day->value == "29");
  CHECK(first_day->role == "muted");
  CHECK(second_day->value == "30");
  CHECK(second_day->role == "today");
}

TEST_CASE("repeat expansion accepts empty arrays and rejects non-arrays") {
  const auto item = shared(text_template(gisland::DataBinding{"item"}));
  const gisland::SceneTemplate list{gisland::TemplateRow{
      .children = {gisland::TemplateRepeat{gisland::DataBinding{"items"}, "item", item}},
  }};

  const auto empty = gisland::instantiate_template(list, {{"items", nlohmann::json::array()}});
  REQUIRE(empty.has_value());
  const auto *row = std::get_if<gisland::Row>(&empty->value);
  REQUIRE(row != nullptr);
  CHECK(row->children.empty());

  const auto mismatch = gisland::instantiate_template(list, {{"items", "none"}});
  REQUIRE_FALSE(mismatch.has_value());
  CHECK(mismatch.error().code == gisland::TemplateErrorCode::repeat_source_mismatch);
  CHECK(mismatch.error().data_path == "/items");
}

TEST_CASE("repeat expansion remains bounded by scene node limits") {
  const auto item = shared(text_template(std::string{"x"}));
  const gisland::SceneTemplate list{gisland::TemplateRow{
      .children = {gisland::TemplateRepeat{gisland::DataBinding{"items"}, "item", item}},
  }};
  const nlohmann::json snapshot{{"items", nlohmann::json::array()}};
  auto oversized = snapshot;
  for (int index = 0; index < 256; ++index) {
    oversized["items"].push_back(index);
  }

  const auto result = gisland::instantiate_template(list, oversized);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == gisland::TemplateErrorCode::invalid_scene);
}

TEST_CASE("module view state replaces compact and expanded views atomically") {
  gisland::ModuleViewState state{text_template(gisland::DataBinding{"compact"}),
                                 text_template(gisland::DataBinding{"expanded"})};
  REQUIRE(state.apply({{"compact", "14:35"}, {"expanded", "July"}, {"old", true}}).has_value());
  const auto *initial_views = state.views().has_value() ? &state.views().value() : nullptr;
  REQUIRE(initial_views != nullptr);

  const auto rejected = state.apply({{"compact", "14:36"}});
  REQUIRE_FALSE(rejected.has_value());
  const auto *preserved = std::get_if<gisland::Text>(&initial_views->compact.value);
  REQUIRE(preserved != nullptr);
  CHECK(preserved->value == "14:35");
  const auto *initial_snapshot = state.snapshot().has_value() ? &state.snapshot().value() : nullptr;
  REQUIRE(initial_snapshot != nullptr);
  CHECK(initial_snapshot->contains("old"));

  REQUIRE(state.apply({{"compact", "14:36"}, {"expanded", "August"}}).has_value());
  const auto *updated_views = state.views().has_value() ? &state.views().value() : nullptr;
  REQUIRE(updated_views != nullptr);
  const auto *replaced = std::get_if<gisland::Text>(&updated_views->compact.value);
  REQUIRE(replaced != nullptr);
  CHECK(replaced->value == "14:36");
  const auto *updated_snapshot = state.snapshot().has_value() ? &state.snapshot().value() : nullptr;
  REQUIRE(updated_snapshot != nullptr);
  CHECK_FALSE(updated_snapshot->contains("old"));
}
