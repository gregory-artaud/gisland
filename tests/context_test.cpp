#include "gisland/context.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr gisland::MonotonicTime epoch{};

[[nodiscard]] gisland::PublishedContext
context(std::string instance_id, std::string context_id, int priority,
        std::optional<gisland::MonotonicTime> expires_at = std::nullopt) {
  return gisland::PublishedContext{{std::move(instance_id), std::move(context_id)},
                                   priority,
                                   expires_at,
                                   gisland::SceneNode{gisland::Text{"compact", "body"}},
                                   gisland::SceneNode{gisland::Text{"expanded", "body"}}};
}

void publish_default(gisland::ContextArbiter &arbiter) {
  arbiter.publish(context("clock", "default", 1000), epoch);
}

} // namespace

TEST_CASE("higher-priority temporary context wins") {
  gisland::ContextArbiter arbiter{{"clock", "default"}};
  publish_default(arbiter);
  arbiter.publish(context("music", "playing", 10), epoch + 1ms);
  arbiter.publish(context("timer", "done", 20), epoch + 2ms);

  const auto *active = arbiter.active(epoch + 2ms);

  REQUIRE(active != nullptr);
  CHECK((active->key == gisland::ContextKey{"timer", "done"}));
}

TEST_CASE("newest publication wins equal priority") {
  gisland::ContextArbiter arbiter{{"clock", "default"}};
  publish_default(arbiter);
  arbiter.publish(context("music", "playing", 10), epoch + 1ms);
  arbiter.publish(context("timer", "running", 10), epoch + 2ms);

  REQUIRE(arbiter.active(epoch + 2ms) != nullptr);
  CHECK((arbiter.active(epoch + 2ms)->key == gisland::ContextKey{"timer", "running"}));

  arbiter.publish(context("music", "playing", 10), epoch + 3ms);

  REQUIRE(arbiter.active(epoch + 3ms) != nullptr);
  CHECK((arbiter.active(epoch + 3ms)->key == gisland::ContextKey{"music", "playing"}));
}

TEST_CASE("expiration uses the supplied monotonic deadline") {
  gisland::ContextArbiter arbiter{{"clock", "default"}};
  publish_default(arbiter);
  arbiter.publish(context("timer", "done", 20, epoch + 10ms), epoch);

  REQUIRE(arbiter.active(epoch + 9ms) != nullptr);
  CHECK((arbiter.active(epoch + 9ms)->key == gisland::ContextKey{"timer", "done"}));

  REQUIRE(arbiter.active(epoch + 10ms) != nullptr);
  CHECK((arbiter.active(epoch + 10ms)->key == gisland::ContextKey{"clock", "default"}));
}

TEST_CASE("dismiss removes a context immediately") {
  gisland::ContextArbiter arbiter{{"clock", "default"}};
  publish_default(arbiter);
  arbiter.publish(context("music", "playing", 10), epoch + 1ms);

  arbiter.dismiss({"music", "playing"});

  REQUIRE(arbiter.active(epoch + 1ms) != nullptr);
  CHECK((arbiter.active(epoch + 1ms)->key == gisland::ContextKey{"clock", "default"}));
}

TEST_CASE("default context is a fallback rather than a priority competitor") {
  gisland::ContextArbiter arbiter{{"clock", "default"}};
  publish_default(arbiter);

  REQUIRE(arbiter.active(epoch) != nullptr);
  CHECK((arbiter.active(epoch)->key == gisland::ContextKey{"clock", "default"}));

  arbiter.publish(context("music", "playing", -100), epoch + 1ms);

  REQUIRE(arbiter.active(epoch + 1ms) != nullptr);
  CHECK((arbiter.active(epoch + 1ms)->key == gisland::ContextKey{"music", "playing"}));
}

TEST_CASE("no published or valid default context yields no selection") {
  gisland::ContextArbiter arbiter{{"clock", "default"}};
  CHECK(arbiter.active(epoch) == nullptr);

  arbiter.publish(context("clock", "default", 0, epoch + 1ms), epoch);
  REQUIRE(arbiter.active(epoch) != nullptr);
  CHECK(arbiter.active(epoch + 1ms) == nullptr);
}

TEST_CASE("publishing an already-expired replacement removes the old context") {
  gisland::ContextArbiter arbiter{{"clock", "default"}};
  publish_default(arbiter);
  arbiter.publish(context("music", "playing", 10), epoch);

  arbiter.publish(context("music", "playing", 20, epoch + 1ms), epoch + 1ms);

  REQUIRE(arbiter.active(epoch + 1ms) != nullptr);
  CHECK((arbiter.active(epoch + 1ms)->key == gisland::ContextKey{"clock", "default"}));
}

TEST_CASE("all contexts owned by a stopped instance are removed together") {
  gisland::ContextArbiter arbiter{{"clock", "default"}};
  publish_default(arbiter);
  arbiter.publish(context("music", "playing", 10), epoch + 1ms);
  arbiter.publish(context("music", "paused", 20), epoch + 2ms);
  arbiter.publish(context("timer", "running", 15), epoch + 3ms);

  arbiter.dismiss_instance("music");

  REQUIRE(arbiter.active(epoch + 3ms) != nullptr);
  CHECK((arbiter.active(epoch + 3ms)->key == gisland::ContextKey{"timer", "running"}));

  arbiter.dismiss_instance("timer");
  REQUIRE(arbiter.active(epoch + 3ms) != nullptr);
  CHECK((arbiter.active(epoch + 3ms)->key == gisland::ContextKey{"clock", "default"}));
}

TEST_CASE("activation selects the best context within one owner and pins it globally") {
  gisland::ContextArbiter arbiter{{"clock", "default"}};
  publish_default(arbiter);
  arbiter.publish(context("music", "low", 5), epoch + 1ms);
  arbiter.publish(context("music", "old", 10), epoch + 2ms);
  arbiter.publish(context("music", "new", 10), epoch + 3ms);
  arbiter.publish(context("timer", "urgent", 100), epoch + 4ms);

  const auto activated = arbiter.activate("music", std::nullopt, epoch + 4ms);
  REQUIRE(activated.has_value());
  CHECK((*activated == gisland::ContextKey{"music", "new"}));
  REQUIRE(arbiter.active(epoch + 4ms) != nullptr);
  CHECK((arbiter.active(epoch + 4ms)->key == gisland::ContextKey{"music", "new"}));
}

TEST_CASE("activation duration and natural expiration restore normal arbitration") {
  gisland::ContextArbiter arbiter{{"clock", "default"}};
  publish_default(arbiter);
  arbiter.publish(context("music", "playing", 10, epoch + 20ms), epoch);
  arbiter.publish(context("timer", "urgent", 100), epoch + 1ms);

  REQUIRE(arbiter.activate("music", epoch + 10ms, epoch + 1ms).has_value());
  REQUIRE(arbiter.active(epoch + 9ms) != nullptr);
  CHECK((arbiter.active(epoch + 9ms)->key == gisland::ContextKey{"music", "playing"}));
  REQUIRE(arbiter.active(epoch + 10ms) != nullptr);
  CHECK((arbiter.active(epoch + 10ms)->key == gisland::ContextKey{"timer", "urgent"}));

  REQUIRE(arbiter.activate("music", std::nullopt, epoch + 11ms).has_value());
  REQUIRE(arbiter.active(epoch + 19ms) != nullptr);
  CHECK((arbiter.active(epoch + 19ms)->key == gisland::ContextKey{"music", "playing"}));
  REQUIRE(arbiter.active(epoch + 20ms) != nullptr);
  CHECK((arbiter.active(epoch + 20ms)->key == gisland::ContextKey{"timer", "urgent"}));
}

TEST_CASE("activation follows same-key updates and clears on dismissal or owner removal") {
  gisland::ContextArbiter arbiter{{"clock", "default"}};
  publish_default(arbiter);
  arbiter.publish(context("music", "playing", 10), epoch);
  REQUIRE(arbiter.activate("music", std::nullopt, epoch).has_value());

  arbiter.publish(context("music", "playing", 20), epoch + 1ms);
  REQUIRE(arbiter.active(epoch + 1ms) != nullptr);
  CHECK(arbiter.active(epoch + 1ms)->priority == 20);

  CHECK_FALSE(arbiter.dismiss_active("different", epoch + 1ms));
  CHECK(arbiter.dismiss_active("playing", epoch + 1ms));
  REQUIRE(arbiter.active(epoch + 1ms) != nullptr);
  CHECK((arbiter.active(epoch + 1ms)->key == gisland::ContextKey{"clock", "default"}));

  arbiter.publish(context("music", "playing", 10), epoch + 2ms);
  REQUIRE(arbiter.activate("music", std::nullopt, epoch + 2ms).has_value());
  arbiter.dismiss_instance("music");
  REQUIRE(arbiter.active(epoch + 2ms) != nullptr);
  CHECK((arbiter.active(epoch + 2ms)->key == gisland::ContextKey{"clock", "default"}));
}

TEST_CASE("rejected activation preserves the existing pin") {
  gisland::ContextArbiter arbiter{{"clock", "default"}};
  publish_default(arbiter);
  arbiter.publish(context("music", "playing", 10), epoch);
  REQUIRE(arbiter.activate("music", std::nullopt, epoch).has_value());

  CHECK_FALSE(arbiter.activate("missing", std::nullopt, epoch).has_value());
  REQUIRE(arbiter.active(epoch) != nullptr);
  CHECK((arbiter.active(epoch)->key == gisland::ContextKey{"music", "playing"}));
}

TEST_CASE("published contexts own immutable image resources") {
  auto published = context("notifications", "download", 10);
  auto pixels =
      std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{255, 0, 0, 255});
  published.resources.push_back(
      gisland::ImageResource{"icon", gisland::ImageFormat::rgba8, 1, 1, pixels});

  gisland::ContextArbiter arbiter{{"clock", "default"}};
  arbiter.publish(std::move(published), epoch);

  const auto *active = arbiter.active(epoch);
  REQUIRE(active != nullptr);
  REQUIRE(active->resources.size() == 1);
  CHECK(active->resources.front().pixels == pixels);
  CHECK(active->resources.front().pixels->at(0) == 255);
}
