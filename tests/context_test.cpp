#include "gisland/context.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <utility>

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
