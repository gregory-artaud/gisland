#pragma once

#include "gisland/bootstrap.hpp"
#include "gisland/module_supervisor.hpp"

#include <expected>
#include <string>
#include <vector>

namespace gisland {

enum class ModuleReloadKind {
  unchanged,
  view_updated,
  added,
  removed,
  enabled,
  disabled,
  process_modified
};

struct ModuleReloadChange {
  std::string instance_id;
  ModuleReloadKind kind;
  bool starts_process;

  bool operator==(const ModuleReloadChange &) const = default;
};

struct ReloadPlan {
  AppConfig candidate;
  std::vector<ModuleReloadChange> changes;
  std::vector<ModuleStartRequest> start_requests;
  SupervisorReconfiguration supervisor;
};

struct ReloadPlanError {
  std::string message;
};

[[nodiscard]] std::expected<RuntimeBootstrap, BootstrapError>
load_reload_candidate(const RuntimeBootstrap &current);
[[nodiscard]] std::expected<ReloadPlan, ReloadPlanError> plan_reload(const AppConfig &current,
                                                                     const AppConfig &candidate,
                                                                     std::string locale,
                                                                     std::string timezone);

} // namespace gisland
