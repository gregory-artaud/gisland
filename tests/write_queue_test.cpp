#include "gisland/write_queue.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

TEST_CASE("write queue preserves FIFO records across partial writes") {
  gisland::WriteQueue queue;
  const std::string first = R"({"sequence":1})"
                            "\n";
  const std::string second = R"({"sequence":2})"
                             "\n";

  REQUIRE(queue.push(first).has_value());
  REQUIRE(queue.push(second).has_value());
  CHECK(queue.wants_write());
  CHECK(queue.message_count() == 2);
  CHECK(queue.pending_bytes() == first.size() + second.size());
  CHECK(std::string_view{queue.front_span()} == first);

  REQUIRE(queue.consume(5).has_value());
  CHECK(std::string_view{queue.front_span()} == std::string_view{first}.substr(5));
  CHECK(queue.message_count() == 2);
  CHECK(queue.pending_bytes() == first.size() + second.size() - 5);

  REQUIRE(queue.consume(first.size() - 5).has_value());
  CHECK(queue.message_count() == 1);
  CHECK(std::string_view{queue.front_span()} == second);

  REQUIRE(queue.consume(second.size()).has_value());
  CHECK_FALSE(queue.wants_write());
  CHECK(queue.front_span().empty());
  CHECK(queue.message_count() == 0);
  CHECK(queue.pending_bytes() == 0);
}

TEST_CASE("write queue accepts only one complete JSONL frame") {
  gisland::WriteQueue queue;
  const std::array invalid_records{
      std::string{},
      std::string{"{}"},
      std::string{"{}\n{}\n"},
      std::string{"{\"x\":\"a\0b\"}\n", 12},
  };

  for (const auto &record : invalid_records) {
    const auto result = queue.push(record);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == gisland::WriteQueueError::invalid_record);
  }

  REQUIRE(queue.push("{}\n").has_value());
}

TEST_CASE("write queue enforces the 256-message limit") {
  gisland::WriteQueue queue;

  for (std::size_t index = 0; index < 256; ++index) {
    REQUIRE(queue.push("{}\n").has_value());
  }
  const auto overflow = queue.push("{}\n");
  REQUIRE_FALSE(overflow.has_value());
  CHECK(overflow.error() == gisland::WriteQueueError::message_limit);
}

TEST_CASE("write queue enforces the one-MiB pending-byte limit") {
  constexpr std::size_t limit = 1024U * 1024U;
  gisland::WriteQueue queue;
  std::string maximum_record(limit - 1, 'x');
  maximum_record.push_back('\n');

  REQUIRE(queue.push(maximum_record).has_value());
  CHECK(queue.pending_bytes() == limit);

  const auto overflow = queue.push("{}\n");
  REQUIRE_FALSE(overflow.has_value());
  CHECK(overflow.error() == gisland::WriteQueueError::byte_limit);
}

TEST_CASE("write queue rejects consumption beyond the current record") {
  gisland::WriteQueue queue;
  REQUIRE(queue.push("{}\n").has_value());

  const auto result = queue.consume(4);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == gisland::WriteQueueError::invalid_consume);
  CHECK(queue.pending_bytes() == 3);
}
