local command = require("command")
local ui = gisland.ui

local options = {
  step_percent = 5,
  maximum_percent = 150,
  hud_duration_ms = 1500,
}
local active_context

local function require_result(result, operation, timeout_seconds)
  if result.ok then
    return result.output
  end
  local detail
  if result.timed_out then
    detail = "timed out after " .. timeout_seconds .. " seconds"
  else
    detail = result.output:gsub("%s+$", "")
    if detail == "" then
      detail = "exited with status " .. tostring(result.status)
    end
  end
  error(operation .. " failed: " .. detail, 2)
end

local function round_half_even(value)
  local lower = math.floor(value)
  local fraction = value - lower
  if fraction < 0.5 then
    return lower
  end
  if fraction > 0.5 then
    return lower + 1
  end
  return lower % 2 == 0 and lower or lower + 1
end

local function parse_volume(output)
  local total = 0
  local count = 0
  for value in output:gmatch("/%s*(%d+)%%") do
    total = total + tonumber(value)
    count = count + 1
  end
  if count == 0 then
    error("pactl returned an invalid sink volume", 2)
  end
  return round_half_even(total / count)
end

local function parse_mute(output)
  local value = output:match("^%s*Mute:%s*(yes)%s*$")
  if value ~= nil then
    return true
  end
  value = output:match("^%s*Mute:%s*(no)%s*$")
  if value ~= nil then
    return false
  end
  error("pactl returned an invalid sink mute state", 2)
end

local function read_state()
  local volume = parse_volume(require_result(command.get_sink_volume(), "pactl get-sink-volume", "2.0"))
  local muted = parse_mute(require_result(command.get_sink_mute(), "pactl get-sink-mute", "2.0"))
  return { volume_percent = volume, muted = muted }
end

local function set_volume(percent)
  require_result(command.set_sink_volume(percent), "pactl set-sink-volume", "2.0")
end

local function set_muted(muted)
  require_result(command.set_sink_mute(muted), "pactl set-sink-mute", "2.0")
end

local function close_expanded()
  local result = command.close()
  if result.ok then
    return
  end
  local detail
  if result.timed_out then
    detail = "timed out after 0.5 seconds"
  else
    detail = result.output:gsub("%s+$", "")
    if detail == "" then
      detail = "exited with status " .. tostring(result.status)
    end
  end
  gisland.log("error", "gisland-audio: gislandctl close failed: " .. detail)
end

local function replace(context_id, compact, style)
  if active_context ~= nil and active_context ~= context_id then
    gisland.dismiss(active_context)
  end
  gisland.publish {
    context_id = context_id,
    priority = 80,
    expires_in_ms = options.hud_duration_ms,
    views = { compact = compact },
    presentation = { compact_style = style },
  }
  active_context = context_id
  gisland.defer(close_expanded)
end

local function publish_mute(state)
  replace(
    "audio-mute",
    ui.icon {
      name = state.muted and "volume-muted" or "volume-high",
      role = "hud-mute-icon",
      accessible_label = state.muted and "Muted" or "Unmuted",
    },
    "hud-symbol"
  )
end

local function clamped_ratio(percent)
  return math.max(0.0, math.min(percent / options.maximum_percent, 1.0))
end

local function publish_volume(before, after)
  local icon
  if after.muted or after.volume_percent == 0 then
    icon = "volume-muted"
  elseif after.volume_percent <= options.maximum_percent / 2 then
    icon = "volume-low"
  else
    icon = "volume-high"
  end
  replace(
    "audio-volume",
    ui.row {
      gap = "small",
      children = {
        ui.icon {
          name = icon,
          role = "hud-volume-icon",
          accessible_label = "Volume " .. tostring(after.volume_percent) .. " percent",
        },
        ui.progress {
          value = clamped_ratio(after.volume_percent),
          transition_from = clamped_ratio(before.volume_percent),
          state = "foreground",
        },
      },
    },
    "hud-meter"
  )
end

local function change_volume(delta)
  local before = read_state()
  local target = math.max(0, math.min(before.volume_percent + delta, options.maximum_percent))
  if before.muted then
    set_muted(false)
  end
  set_volume(target)
  publish_volume(before, read_state())
  return true
end

local function toggle_mute()
  local before = read_state()
  set_muted(not before.muted)
  publish_mute(read_state())
  return true
end

local function validate_integer(config, name, minimum, maximum)
  local value = config[name]
  if value == nil then
    return options[name]
  end
  if math.type(value) ~= "integer" then
    error(name .. " must be an integer")
  end
  if value < minimum or value > maximum then
    error(name .. " must be between " .. minimum .. " and " .. maximum)
  end
  return value
end

return gisland.module {
  init = function(config)
    local unknown = {}
    for name in pairs(config) do
      if options[name] == nil then
        unknown[#unknown + 1] = name
      end
    end
    table.sort(unknown)
    if #unknown > 0 then
      error("unknown audio option: " .. unknown[1])
    end
    options.step_percent = validate_integer(config, "step_percent", 1, 25)
    options.maximum_percent = validate_integer(config, "maximum_percent", 100, 200)
    options.hud_duration_ms = validate_integer(config, "hud_duration_ms", 100, 60000)
  end,
  actions = {
    ["volume-up"] = function()
      return change_volume(options.step_percent)
    end,
    ["volume-down"] = function()
      return change_volume(-options.step_percent)
    end,
    ["toggle-mute"] = toggle_mute,
  },
}
