#pragma once

#include "gisland/config.hpp"
#include "gisland/context.hpp"
#include "gisland/island.hpp"
#include "gisland/module_supervisor.hpp"
#include "gisland/scene_template.hpp"

#include <cstdint>
#include <expected>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace gisland {

inline constexpr std::string_view configured_context_id = "configured";

enum class RuntimeErrorCode { unknown_instance, missing_view, invalid_snapshot };

struct RuntimeError {
  RuntimeErrorCode code;
  std::string instance_id;
  std::string message;
};

struct RuntimeSelection {
  const PublishedContext *context;
  std::uint64_t revision;
};

struct VisibilityUpdate {
  std::string instance_id;
  Visibility visibility;

  bool operator==(const VisibilityUpdate &) const = default;
};

[[nodiscard]] ModuleStartRequest make_module_start_request(const ModuleInstanceConfig &config,
                                                           std::string locale,
                                                           std::string timezone);

class RuntimeCoordinator final {
public:
  explicit RuntimeCoordinator(const AppConfig &config);

  [[nodiscard]] std::expected<void, RuntimeError> consume(const SupervisorEvent &event);
  [[nodiscard]] RuntimeSelection active(MonotonicTime now);
  void reject(const ContextKey &key);
  [[nodiscard]] std::vector<VisibilityUpdate> visibility_updates(MonotonicTime now,
                                                                 IslandMode mode);

private:
  [[nodiscard]] std::expected<void, RuntimeError> consume_message(const ModuleMessageEvent &event);

  ContextArbiter arbiter_;
  std::map<std::string, ModuleViewState> views_;
  std::vector<std::string> enabled_instances_;
  std::set<std::string> ready_instances_;
  std::map<std::string, Visibility> visibility_;
  std::uint64_t revision_{};
};

} // namespace gisland
