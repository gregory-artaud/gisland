#include "gisland/platform_host.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string_view>
#include <utility>

namespace {

class FakePlatformHost final : public gisland::PlatformHost {
public:
  explicit FakePlatformHost(bool &destroyed) : destroyed_(destroyed) {}
  ~FakePlatformHost() override { destroyed_ = true; }

  [[nodiscard]] std::expected<gisland::OutputSelection, gisland::PlatformError>
  select_output(std::string_view requested_name) const override {
    if (requested_name == "error") {
      return std::unexpected(gisland::PlatformError{gisland::PlatformOperation::query_outputs,
                                                    gisland::PlatformErrorSeverity::recoverable,
                                                    "output query failed"});
    }
    return gisland::OutputSelection{
        gisland::DisplayOutput{std::string{requested_name}, 0, 0, 1920, 1080, true}, false};
  }

  [[nodiscard]] std::expected<void, gisland::PlatformError>
  update_input_region(const gisland::InputRegion &region) const override {
    last_region = region;
    return {};
  }

  [[nodiscard]] std::expected<std::vector<gisland::PlatformEvent>, gisland::PlatformError>
  poll_events() override {
    return std::vector{gisland::PlatformEvent{gisland::PlatformEventKind::output_topology_changed}};
  }

  mutable gisland::InputRegion last_region{};

private:
  bool &destroyed_;
};

} // namespace

TEST_CASE("platform host exposes neutral output region event and error contracts") {
  bool destroyed = false;
  {
    gisland::PlatformHostPtr host = std::make_unique<FakePlatformHost>(destroyed);
    const auto selected = host->select_output("DP-1");
    REQUIRE(selected.has_value());
    CHECK(selected->output.name == "DP-1");

    const gisland::InputRegion region{gisland::IslandGeometry{230.0F, 32.0F, 16.0F},
                                      gisland::IslandPlacement{20.0F, 10.0F}};
    REQUIRE(host->update_input_region(region).has_value());
    const auto &fake = static_cast<const FakePlatformHost &>(*host);
    CHECK(fake.last_region.geometry == region.geometry);
    CHECK(fake.last_region.placement == region.placement);

    const auto events = host->poll_events();
    REQUIRE(events.has_value());
    REQUIRE(events->size() == 1);
    CHECK(events->front().kind == gisland::PlatformEventKind::output_topology_changed);

    const auto failed = host->select_output("error");
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().operation == gisland::PlatformOperation::query_outputs);
    CHECK(failed.error().severity == gisland::PlatformErrorSeverity::recoverable);
    CHECK(failed.error().message == "output query failed");
  }
  CHECK(destroyed);
}
