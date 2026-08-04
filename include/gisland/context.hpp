#pragma once

#include "gisland/scene.hpp"

#include <chrono>
#include <compare>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>

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

enum class ContextActivationError { unavailable_instance };

class ContextArbiter {
public:
  explicit ContextArbiter(ContextKey default_context);

  void publish(PublishedContext context, MonotonicTime now);
  void dismiss(const ContextKey &key);
  void dismiss_instance(std::string_view instance_id);
  void set_default(ContextKey default_context);
  [[nodiscard]] std::expected<ContextKey, ContextActivationError>
  activate(std::string_view instance_id, std::optional<MonotonicTime> deadline, MonotonicTime now);
  [[nodiscard]] bool dismiss_active(std::string_view context_id, MonotonicTime now);
  [[nodiscard]] bool available(std::string_view instance_id, MonotonicTime now);
  [[nodiscard]] const PublishedContext *active(MonotonicTime now);

private:
  struct Entry {
    PublishedContext context;
    std::uint64_t sequence;
  };
  struct Activation {
    ContextKey key;
    std::optional<MonotonicTime> deadline;
  };

  void expire(MonotonicTime now);
  [[nodiscard]] const Entry *best_for_instance(std::string_view instance_id) const;

  ContextKey default_context_;
  std::uint64_t sequence_{0};
  std::map<ContextKey, Entry> contexts_;
  std::optional<Activation> activation_;
};

} // namespace gisland
