#pragma once

#include "gisland/scene.hpp"

#include <chrono>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace gisland {

using MonotonicTime = std::chrono::steady_clock::time_point;

struct ContextKey {
  std::string instance_id;
  std::string context_id;

  auto operator<=>(const ContextKey &) const = default;
};

struct PublishedContext {
  ContextKey key;
  int priority;
  std::optional<MonotonicTime> expires_at;
  SceneNode compact;
  std::optional<SceneNode> expanded;
};

class ContextArbiter {
public:
  explicit ContextArbiter(ContextKey default_context);

  void publish(PublishedContext context, MonotonicTime now);
  void dismiss(const ContextKey &key);
  [[nodiscard]] const PublishedContext *active(MonotonicTime now);

private:
  struct Entry {
    PublishedContext context;
    std::uint64_t sequence;
  };

  ContextKey default_context_;
  std::uint64_t sequence_{0};
  std::map<ContextKey, Entry> contexts_;
};

} // namespace gisland
