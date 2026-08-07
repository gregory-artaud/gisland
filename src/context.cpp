#include "gisland/context.hpp"

#include <utility>

namespace gisland {

ContextArbiter::ContextArbiter(ContextKey default_context)
    : default_context_(std::move(default_context)), compact_default_(default_context_.instance_id),
      expanded_default_(default_context_.instance_id) {}

ContextArbiter::ContextArbiter(std::string compact_default, std::string expanded_default)
    : default_context_{compact_default, {}}, compact_default_(std::move(compact_default)),
      expanded_default_(std::move(expanded_default)) {}

void ContextArbiter::publish(PublishedContext context, MonotonicTime now) {
  const ContextKey key = context.key;
  if (context.expires_at.has_value() && *context.expires_at <= now) {
    contexts_.erase(key);
    if (activation_ && activation_->key == key) {
      activation_.reset();
    }
    return;
  }

  ++sequence_;
  contexts_.insert_or_assign(key, Entry{std::move(context), sequence_});
}

void ContextArbiter::dismiss(const ContextKey &key) {
  contexts_.erase(key);
  if (activation_ && activation_->key == key) {
    activation_.reset();
  }
}

void ContextArbiter::dismiss_instance(std::string_view instance_id) {
  for (auto iterator = contexts_.begin(); iterator != contexts_.end();) {
    if (iterator->first.instance_id == instance_id) {
      iterator = contexts_.erase(iterator);
    } else {
      ++iterator;
    }
  }
  if (activation_ && activation_->key.instance_id == instance_id) {
    activation_.reset();
  }
}

void ContextArbiter::set_default(ContextKey default_context) {
  default_context_ = std::move(default_context);
  compact_default_ = default_context_.instance_id;
  expanded_default_ = default_context_.instance_id;
}

void ContextArbiter::set_defaults(std::string compact_default, std::string expanded_default) {
  default_context_ = ContextKey{compact_default, {}};
  compact_default_ = std::move(compact_default);
  expanded_default_ = std::move(expanded_default);
}

void ContextArbiter::expire(MonotonicTime now) {
  for (auto iterator = contexts_.begin(); iterator != contexts_.end();) {
    const auto &expiration = iterator->second.context.expires_at;
    if (expiration.has_value() && *expiration <= now) {
      iterator = contexts_.erase(iterator);
    } else {
      ++iterator;
    }
  }
  if (activation_ && ((activation_->deadline && *activation_->deadline <= now) ||
                      !contexts_.contains(activation_->key))) {
    activation_.reset();
  }
}

const ContextArbiter::Entry *ContextArbiter::best_for_instance(std::string_view instance_id) const {
  const Entry *best = nullptr;
  for (const auto &[key, entry] : contexts_) {
    if (key.instance_id != instance_id) {
      continue;
    }
    if (best == nullptr || entry.context.priority > best->context.priority ||
        (entry.context.priority == best->context.priority && entry.sequence > best->sequence)) {
      best = &entry;
    }
  }
  return best;
}

bool ContextArbiter::contributes(const PublishedContext &context, ViewSlot slot) {
  return slot == ViewSlot::compact ? context.compact.has_value() : context.expanded.has_value();
}

const ContextArbiter::Entry *ContextArbiter::best_for_instance(std::string_view instance_id,
                                                               ViewSlot slot) const {
  const Entry *best = nullptr;
  for (const auto &[key, entry] : contexts_) {
    if (key.instance_id != instance_id || !contributes(entry.context, slot)) {
      continue;
    }
    if (best == nullptr || entry.context.priority > best->context.priority ||
        (entry.context.priority == best->context.priority && entry.sequence > best->sequence)) {
      best = &entry;
    }
  }
  return best;
}

std::expected<ContextKey, ContextActivationError>
ContextArbiter::activate(std::string_view instance_id, std::optional<MonotonicTime> deadline,
                         MonotonicTime now) {
  expire(now);
  const Entry *best = best_for_instance(instance_id);
  if (best == nullptr) {
    return std::unexpected(ContextActivationError::unavailable_instance);
  }
  activation_ = Activation{best->context.key, deadline};
  return best->context.key;
}

bool ContextArbiter::dismiss_active(std::string_view context_id, MonotonicTime now) {
  const PublishedContext *selected = active(now);
  if (selected == nullptr || selected->key.context_id != context_id) {
    return false;
  }
  const ContextKey key = selected->key;
  dismiss(key);
  return true;
}

bool ContextArbiter::available(std::string_view instance_id, MonotonicTime now) {
  expire(now);
  return best_for_instance(instance_id) != nullptr;
}

const PublishedContext *ContextArbiter::find(const ContextKey &key, MonotonicTime now) {
  expire(now);
  const auto iterator = contexts_.find(key);
  return iterator == contexts_.end() ? nullptr : &iterator->second.context;
}

const PublishedContext *ContextArbiter::active(MonotonicTime now) {
  return active(ViewSlot::compact, now);
}

const PublishedContext *ContextArbiter::active(ViewSlot slot, MonotonicTime now) {
  expire(now);
  if (activation_ && contributes(contexts_.at(activation_->key).context, slot)) {
    return &contexts_.at(activation_->key).context;
  }

  const std::string_view fallback = slot == ViewSlot::compact ? std::string_view{compact_default_}
                                                              : std::string_view{expanded_default_};
  const Entry *temporary_entry = nullptr;
  for (const auto &[key, entry] : contexts_) {
    if (!contributes(entry.context, slot) || key.instance_id == fallback) {
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
  if (const Entry *fallback_entry = best_for_instance(fallback, slot); fallback_entry != nullptr) {
    return &fallback_entry->context;
  }
  return nullptr;
}

} // namespace gisland
