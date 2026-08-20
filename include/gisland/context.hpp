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
#include <vector>

namespace gisland {

using MonotonicTime = std::chrono::steady_clock::time_point;

struct ContextKey {
  std::string instance_id;
  std::string context_id;

  auto operator<=>(const ContextKey &) const = default;
};

enum class ViewSlot { compact, expanded };

enum class ContentTransition { crossfade, slide_left, slide_right };

struct ViewTransitions {
  std::optional<ContentTransition> compact;
  std::optional<ContentTransition> expanded;
};

enum class Reveal { expanded };

struct PresentationIntent {
  std::optional<Reveal> reveal;
  std::optional<std::chrono::milliseconds> duration;
  std::optional<std::string> compact_style;
};

struct PublishedContext {
  ContextKey key;
  int priority;
  std::optional<MonotonicTime> expires_at;
  std::optional<SceneNode> compact;
  std::optional<SceneNode> expanded;
  std::vector<ImageResource> resources{};
  std::optional<PresentationIntent> presentation{};
  std::uint64_t revision{};
  bool fallback_only{};
  ViewTransitions transitions{};
};

enum class ContextActivationError { unavailable_instance };

class ContextArbiter {
public:
  explicit ContextArbiter(ContextKey default_context);
  ContextArbiter(std::string compact_default, std::string expanded_default);

  void publish(PublishedContext context, MonotonicTime now);
  void dismiss(const ContextKey &key);
  void dismiss_instance(std::string_view instance_id);
  void set_default(ContextKey default_context);
  void set_defaults(std::string compact_default, std::string expanded_default);
  [[nodiscard]] std::expected<ContextKey, ContextActivationError>
  activate(std::string_view instance_id, std::optional<MonotonicTime> deadline, MonotonicTime now);
  void set_activation_held(bool held);
  [[nodiscard]] bool dismiss_active(std::string_view context_id, MonotonicTime now);
  [[nodiscard]] bool available(std::string_view instance_id, MonotonicTime now);
  [[nodiscard]] const PublishedContext *find(const ContextKey &key, MonotonicTime now);
  [[nodiscard]] const PublishedContext *active(MonotonicTime now);
  [[nodiscard]] const PublishedContext *active(ViewSlot slot, MonotonicTime now);

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
  [[nodiscard]] const Entry *best_for_instance(std::string_view instance_id, ViewSlot slot) const;
  [[nodiscard]] static bool contributes(const PublishedContext &context, ViewSlot slot);

  ContextKey default_context_;
  std::string compact_default_;
  std::string expanded_default_;
  std::uint64_t sequence_{0};
  std::map<ContextKey, Entry> contexts_;
  std::optional<Activation> activation_;
  bool activation_held_{};
};

} // namespace gisland
