#include "gisland/line_buffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] std::span<const std::byte> bytes(std::string_view value) {
  return std::as_bytes(std::span{value.data(), value.size()});
}

} // namespace

TEST_CASE("line buffer supports partial, multiple, and final unterminated lines") {
  auto buffer = gisland::LineBuffer::protocol();

  const auto partial = buffer.append(bytes("first par"));
  REQUIRE(partial.has_value());
  CHECK(partial->empty());

  const auto complete = buffer.append(bytes("t\r\nsecond\nthird"));
  REQUIRE(complete.has_value());
  REQUIRE(complete->size() == 2);
  CHECK((*complete)[0].text == "first part");
  CHECK_FALSE((*complete)[0].truncated);
  CHECK((*complete)[1].text == "second");

  const auto final = buffer.finish();
  REQUIRE(final.has_value());
  REQUIRE(final->has_value());
  const auto final_line = final.value().value_or(gisland::BufferedLine{});
  CHECK(final_line.text == "third");
  CHECK_FALSE(final_line.truncated);

  const auto repeated_finish = buffer.finish();
  REQUIRE(repeated_finish.has_value());
  CHECK_FALSE(repeated_finish->has_value());

  const auto after_finish = buffer.append(bytes("late\n"));
  REQUIRE_FALSE(after_finish.has_value());
  CHECK(after_finish.error().code == gisland::LineBufferErrorCode::finished);
}

TEST_CASE("line buffer preserves UTF-8 bytes split across reads") {
  auto buffer = gisland::LineBuffer::protocol();
  const std::string input = "caf\u00e9\n";

  const auto first = buffer.append(bytes(std::string_view{input}.substr(0, 4)));
  REQUIRE(first.has_value());
  CHECK(first->empty());

  const auto second = buffer.append(bytes(std::string_view{input}.substr(4)));
  REQUIRE(second.has_value());
  REQUIRE(second->size() == 1);
  CHECK(second->front().text == "caf\u00e9");
}

TEST_CASE("protocol lines reject embedded NUL and overflow") {
  SECTION("embedded NUL") {
    auto buffer = gisland::LineBuffer::protocol();
    const std::string input{"before\0after\n", 13};
    const auto result = buffer.append(bytes(input));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LineBufferErrorCode::embedded_nul);
  }

  SECTION("line length") {
    auto buffer = gisland::LineBuffer::protocol(4);
    const auto result = buffer.append(bytes("12345"));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gisland::LineBufferErrorCode::line_too_long);
  }
}

TEST_CASE("protocol records are bounded at 8 MiB") {
  CHECK(gisland::LineBuffer::default_protocol_limit == std::size_t{8} * 1024U * 1024U);
}

TEST_CASE("stderr lines truncate explicitly at 64 KiB and continue decoding") {
  constexpr std::size_t limit = std::size_t{64} * 1024U;
  auto buffer = gisland::LineBuffer::standard_error();
  const std::string input = std::string(limit, 'x') + std::string(128, 'y') + "\nnext\n";

  const auto result = buffer.append(bytes(input));

  REQUIRE(result.has_value());
  REQUIRE(result->size() == 2);
  CHECK((*result)[0].text.size() == limit);
  CHECK((*result)[0].text == std::string(limit, 'x'));
  CHECK((*result)[0].truncated);
  CHECK((*result)[1].text == "next");
  CHECK_FALSE((*result)[1].truncated);
}

TEST_CASE("empty lines are retained and terminal newline creates no extra final line") {
  auto buffer = gisland::LineBuffer::protocol();

  const auto lines = buffer.append(bytes("\n\n"));
  REQUIRE(lines.has_value());
  REQUIRE(lines->size() == 2);
  CHECK((*lines)[0].text.empty());
  CHECK((*lines)[1].text.empty());

  const auto final = buffer.finish();
  REQUIRE(final.has_value());
  CHECK_FALSE(final->has_value());
}
