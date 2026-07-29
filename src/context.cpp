#include "gisland/context.hpp"

#include <utility>

namespace gisland {

ContextArbiter::ContextArbiter(ContextKey default_context)
    : default_context_(std::move(default_context)) {}

void ContextArbiter::publish(PublishedContext context, MonotonicTime now) {
  const ContextKey key = context.key;
  if (context.expires_at.has_value() && *context.expires_at <= now) {
    contexts_.erase(key);
    return;
  }

  ++sequence_;
  contexts_.insert_or_assign(key, Entry{std::move(context), sequence_});
}

void ContextArbiter::dismiss(const ContextKey &key) { contexts_.erase(key); }

void ContextArbiter::dismiss_instance(std::string_view instance_id) {
  for (auto iterator = contexts_.begin(); iterator != contexts_.end();) {
    if (iterator->first.instance_id == instance_id) {
      iterator = contexts_.erase(iterator);
    } else {
      ++iterator;
    }
  }
}

const PublishedContext *ContextArbiter::active(MonotonicTime now) {
  for (auto iterator = contexts_.begin(); iterator != contexts_.end();) {
    const auto &expiration = iterator->second.context.expires_at;
    if (expiration.has_value() && *expiration <= now) {
      iterator = contexts_.erase(iterator);
    } else {
      ++iterator;
    }
  }

  const Entry *default_entry = nullptr;
  const Entry *temporary_entry = nullptr;
  for (const auto &[key, entry] : contexts_) {
    if (key == default_context_) {
      default_entry = &entry;
      continue;
    }
    if (temporary_entry == nullptr || entry.context.priority > temporary_entry->context.priority ||
        (entry.context.priority == temporary_entry->context.priority &&
         entry.sequence > temporary_entry->sequence)) {
      temporary_entry = &entry;
    }
  }

  if (temporary_entry != nullptr) {
    return &temporary_entry->context;
  }
  if (default_entry != nullptr) {
    return &default_entry->context;
  }
  return nullptr;
}

} // namespace gisland
