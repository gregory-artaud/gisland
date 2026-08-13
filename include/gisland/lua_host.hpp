#pragma once

#include <nlohmann/json_fwd.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gisland {

enum class LuaHostErrorCode {
  state_creation_failed,
  file_error,
  syntax_error,
  runtime_error,
  invalid_definition,
  missing_definition,
  invalid_return,
  protocol_error,
  callback_error,
  value_error,
  output_error,
};

struct LuaHostError {
  LuaHostErrorCode code;
  std::string message;
};

struct LuaModuleDefinition {
  std::optional<std::string> every;
  bool has_init{};
  bool has_update{};
  bool has_visibility{};
  bool has_shutdown{};
  std::vector<std::string> actions;
};

enum class LuaHostState { running, stopped };

class LuaHost {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  using Emit = std::function<std::expected<void, LuaHostError>(nlohmann::json)>;

  [[nodiscard]] static std::expected<std::unique_ptr<LuaHost>, LuaHostError>
  load(const std::string &entry_path);
  [[nodiscard]] static std::expected<std::chrono::milliseconds, LuaHostError>
  parse_duration(std::string_view value);

  ~LuaHost();
  LuaHost(const LuaHost &) = delete;
  LuaHost &operator=(const LuaHost &) = delete;
  LuaHost(LuaHost &&) noexcept;
  LuaHost &operator=(LuaHost &&) noexcept;

  [[nodiscard]] const LuaModuleDefinition &definition() const noexcept;
  [[nodiscard]] std::size_t retained_callback_count() const noexcept;
  [[nodiscard]] int stack_size() const noexcept;
  [[nodiscard]] std::expected<LuaHostState, LuaHostError> handle(const nlohmann::json &record,
                                                                 const Emit &emit, TimePoint now);
  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] bool stopped() const noexcept;
  [[nodiscard]] std::optional<TimePoint> next_deadline() const noexcept;
  [[nodiscard]] std::expected<void, LuaHostError> run_due(TimePoint now, const Emit &emit);

private:
  class Impl;
  explicit LuaHost(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] int lua_host_poll_timeout(std::optional<LuaHost::TimePoint> deadline,
                                        LuaHost::TimePoint now);

} // namespace gisland
