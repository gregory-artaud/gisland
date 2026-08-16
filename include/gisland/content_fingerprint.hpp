#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace gisland {

[[nodiscard]] inline std::string content_fingerprint(std::string_view content) {
  constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
  constexpr std::uint64_t prime = 1099511628211ULL;
  constexpr std::string_view digits = "0123456789abcdef";

  std::uint64_t hash = offset_basis;
  for (const unsigned char byte : content) {
    hash ^= byte;
    hash *= prime;
  }

  std::array<char, 16> result{};
  for (std::size_t index = result.size(); index > 0; --index) {
    result[index - 1] = digits[static_cast<std::size_t>(hash & 0x0FU)];
    hash >>= 4U;
  }
  return {result.data(), result.size()};
}

} // namespace gisland
