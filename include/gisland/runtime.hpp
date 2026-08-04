#pragma once

#include "gisland/config.hpp"
#include "gisland/context.hpp"
#include "gisland/island.hpp"
#include "gisland/module_supervisor.hpp"
#include "gisland/reload.hpp"
#include "gisland/scene_template.hpp"

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gisland {

inline constexpr std::string_view configured_context_id = "configured";

enum class RuntimeErrorCode {
  unknown_instance,
  disabled_instance,
  unavailable_instance,
  unknown_context,
  missing_view,
  invalid_snapshot
};

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

struct RuntimeModuleStatus {
  std::string id;
  bool enabled;
  ModuleState state;
  bool available;

  bool operator==(const RuntimeModuleStatus &) const = default;
};

struct PreparedRuntimeReload {
  ContextArbiter arbiter;
  std::vector<std::pair<std::string, bool>> configured_instances;
  std::map<std::string, ModuleState> module_states;
  std::map<std::string, ModuleViewState> views;
  std::vector<std::string> enabled_instances;
  std::set<std::string> ready_instances;
  std::map<std::string, Visibility> visibility;
  std::uint64_t revision;
};

[[nodiscard]] ModuleStartRequest make_module_start_request(const ModuleInstanceConfig &config,
                                                           std::string locale,
                                                           std::string timezone);

class RuntimeCoordinator final {
public:
  explicit RuntimeCoordinator(const AppConfig &config);

  [[nodiscard]] std::expected<void, RuntimeError> consume(const SupervisorEvent &event);
  [[nodiscard]] RuntimeSelection active(MonotonicTime now);
  [[nodiscard]] std::expected<ContextKey, RuntimeError>
  activate(std::string_view instance_id, std::optional<std::chrono::milliseconds> duration,
           MonotonicTime now);
  [[nodiscard]] std::expected<ContextKey, RuntimeError> dismiss_active(std::string_view context_id,
                                                                       MonotonicTime now);
  [[nodiscard]] std::vector<RuntimeModuleStatus> module_statuses(MonotonicTime now);
  [[nodiscard]] std::expected<PreparedRuntimeReload, RuntimeError>
  prepare_reload(const ReloadPlan &plan) const;
  void commit_reload(PreparedRuntimeReload prepared) noexcept;
  void reject(const ContextKey &key);
  [[nodiscard]] std::vector<VisibilityUpdate> visibility_updates(MonotonicTime now,
                                                                 IslandMode mode);

private:
  [[nodiscard]] std::expected<void, RuntimeError> consume_message(const ModuleMessageEvent &event);

  ContextArbiter arbiter_;
  std::vector<std::pair<std::string, bool>> configured_instances_;
  std::map<std::string, ModuleState> module_states_;
  std::map<std::string, ModuleViewState> views_;
  std::vector<std::string> enabled_instances_;
  std::set<std::string> ready_instances_;
  std::map<std::string, Visibility> visibility_;
  std::uint64_t revision_{};
};

} // namespace gisland
