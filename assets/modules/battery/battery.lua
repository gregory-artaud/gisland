local lgi = require("lgi")
local Gio = lgi.Gio
local GLib = lgi.GLib
local Json = lgi.require("Json", "1.0")

local options = {
  warning_percent = 20,
  persistent_percent = 10,
  critical_percent = 5,
  yellow_percent = 30,
  red_percent = 15,
  preview_duration_ms = 3000,
}
local emitted = {}
local previous_on_battery
local last_snapshot
local last_publish = 0
local active_alert
local root_proxy
local device_proxy
local stopped = false

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

local function duration(seconds)
  if seconds <= 0 then
    return nil
  end
  local minutes = (seconds + 30) // 60
  local hours = minutes // 60
  local remainder = minutes % 60
  if hours > 0 then
    return string.format("%d h %02d", hours, remainder)
  end
  return tostring(remainder) .. " min"
end

local function snapshot_for(reading)
  local percentage = math.max(0.0, math.min(reading.percentage, 100.0))
  local semantic_state
  if percentage > options.yellow_percent then
    semantic_state = "success"
  elseif percentage > options.red_percent then
    semantic_state = "warning"
  else
    semantic_state = "error"
  end
  local estimate = duration(reading.on_battery and reading.time_to_empty or reading.time_to_full)
  local state_text
  if reading.state == "charging" then
    state_text = "Charge"
  elseif reading.state == "fully-charged" then
    state_text = "Chargée"
  elseif reading.on_battery then
    state_text = "Décharge"
  else
    state_text = "Secteur"
  end
  local compact_estimate
  local detail_estimate
  if estimate == nil then
    compact_estimate = "Calcul…"
    detail_estimate = "—"
  elseif reading.on_battery then
    compact_estimate = estimate
    detail_estimate = estimate .. " restantes"
  else
    compact_estimate = estimate
    detail_estimate = estimate .. " avant charge complète"
  end
  local health = "—"
  if reading.energy_full > 0.0 and reading.energy_full_design > 0.0 then
    health = tostring(round_half_even(reading.energy_full / reading.energy_full_design * 100.0)) ..
      " %"
  end
  local power = "—"
  if reading.energy_rate > 0.0 then
    power = string.format("%.1f W", reading.energy_rate):gsub("%.", ",")
  end
  return {
    level = round_half_even(percentage / 100.0 * 10000.0) / 10000.0,
    percent_text = tostring(round_half_even(percentage)) .. " %",
    semantic_state = semantic_state,
    estimate_compact = compact_estimate,
    state_text = state_text,
    estimate_detail = detail_estimate,
    health_text = health,
    power_text = power,
  }
end

local function compact_view(snapshot)
  return {
    type = "row",
    gap = "small",
    children = {
      {
        type = "progress",
        shape = "ring",
        value = snapshot.level,
        state = snapshot.semantic_state,
      },
      { type = "text", value = snapshot.percent_text, role = "compact-primary" },
      { type = "spacer", flexible = true },
      { type = "text", value = snapshot.estimate_compact, role = "compact-secondary" },
    },
  }
end

local function detail_row(label, value)
  return {
    type = "row",
    gap = "small",
    children = {
      { type = "text", value = label, role = "caption" },
      { type = "spacer", flexible = true },
      { type = "text", value = value, role = "body" },
    },
  }
end

local function expanded_view(snapshot, dismissible)
  local children = {
    {
      type = "row",
      gap = "normal",
      children = {
        {
          type = "progress",
          shape = "ring",
          value = snapshot.level,
          state = snapshot.semantic_state,
        },
        { type = "text", value = snapshot.percent_text, role = "title" },
        { type = "spacer", flexible = true },
        { type = "text", value = snapshot.state_text, role = "body" },
      },
    },
    detail_row("Autonomie", snapshot.estimate_detail),
    detail_row("Santé", snapshot.health_text),
    detail_row("Puissance", snapshot.power_text),
  }
  if dismissible then
    children[#children + 1] = {
      type = "button",
      action_id = "dismiss-alert",
      accessible_label = "Dismiss battery alert",
      content = { type = "text", value = "Dismiss", role = "button" },
    }
  end
  return { type = "column", gap = "normal", children = children }
end

local function state_path()
  local root = os.getenv("XDG_STATE_HOME")
  if root == nil or root == "" then
    root = assert(os.getenv("HOME"), "HOME is not set") .. "/.local/state"
  end
  return root .. "/gisland/battery-cycle.json"
end

local function load_state()
  emitted = {}
  previous_on_battery = nil
  local ok, loaded_emitted, loaded_previous = pcall(function()
    local value = assert(GLib.file_get_contents(state_path()))
    local parser = Json.Parser.new()
    assert(parser:load_from_data(value, -1), "invalid JSON")
    local root = parser:get_root()
    assert(tostring(root:get_node_type()) == "OBJECT", "state root must be an object")
    local object = root:get_object()
    assert(object:get_size() == 3, "state must contain exactly three members")

    local version = object:get_member("version")
    local emitted_node = object:get_member("emitted")
    local previous = object:get_member("previous_on_battery")
    assert(version ~= nil and emitted_node ~= nil and previous ~= nil, "state member is missing")
    assert(
      tostring(version:get_node_type()) == "VALUE" and
        tostring(version:get_value_type()) == "gint64" and version:get_int() == 1,
      "unsupported state version"
    )
    assert(tostring(emitted_node:get_node_type()) == "ARRAY", "emitted must be an array")

    local expected = {
      [options.warning_percent] = true,
      [options.persistent_percent] = true,
      [options.critical_percent] = true,
    }
    local parsed_emitted = {}
    local array = emitted_node:get_array()
    for index = 0, array:get_length() - 1 do
      local item = array:get_element(index)
      assert(
        tostring(item:get_node_type()) == "VALUE" and tostring(item:get_value_type()) == "gint64",
        "emitted threshold must be an integer"
      )
      local threshold = math.tointeger(item:get_int())
      assert(expected[threshold], "emitted threshold is not configured")
      parsed_emitted[threshold] = true
    end

    local parsed_previous
    if tostring(previous:get_node_type()) == "NULL" then
      parsed_previous = nil
    else
      assert(
        tostring(previous:get_node_type()) == "VALUE" and
          tostring(previous:get_value_type()) == "gboolean",
        "previous_on_battery must be boolean or null"
      )
      parsed_previous = previous:get_boolean()
    end
    return parsed_emitted, parsed_previous
  end)
  if ok then
    emitted = loaded_emitted
    previous_on_battery = loaded_previous
  end
end

local function save_state()
  local path = state_path()
  local directory = path:match("^(.*)/[^/]+$")
  assert(GLib.mkdir_with_parents(directory, tonumber("700", 8)) == 0, "cannot create battery state directory")
  local thresholds = {}
  for threshold in pairs(emitted) do
    thresholds[#thresholds + 1] = threshold
  end
  table.sort(thresholds)
  local builder = Json.Builder.new()
  builder:begin_object()
  builder:set_member_name("version")
  builder:add_int_value(1)
  builder:set_member_name("emitted")
  builder:begin_array()
  for _, threshold in ipairs(thresholds) do
    builder:add_int_value(threshold)
  end
  builder:end_array()
  builder:set_member_name("previous_on_battery")
  if previous_on_battery == nil then
    builder:add_null_value()
  else
    builder:add_boolean_value(previous_on_battery)
  end
  builder:end_object()

  local generator = Json.Generator.new()
  generator:set_root(builder:get_root())
  local contents = generator:to_data() .. "\n"
  local flags = GLib.FileSetContentsFlags.CONSISTENT + GLib.FileSetContentsFlags.DURABLE
  local saved, message = GLib.file_set_contents_full(path, contents, flags, tonumber("600", 8))
  assert(saved, tostring(message))
  assert(GLib.chmod(path, tonumber("600", 8)) == 0, "cannot set battery state permissions")
end

local function publish_alert(kind, snapshot)
  if active_alert ~= nil then
    gisland.dismiss(active_alert)
  end
  local context_id = "battery-" .. kind
  local persistent = kind == "persistent" or kind == "critical"
  local priority = ({
    plugged = 35,
    unplugged = 35,
    warning = 40,
    persistent = 60,
    critical = 90,
  })[kind]
  local record = {
    context_id = context_id,
    priority = priority,
    views = {
      compact = compact_view(snapshot),
      expanded = expanded_view(snapshot, persistent),
    },
    presentation = { reveal = "expanded" },
  }
  if persistent then
    active_alert = context_id
  else
    record.expires_in_ms = options.preview_duration_ms
    record.presentation.duration_ms = options.preview_duration_ms
    active_alert = nil
  end
  gisland.publish(record)
end

local function observe(reading)
  local alerts = {}
  local first = previous_on_battery == nil
  if not first and reading.on_battery ~= previous_on_battery then
    if reading.on_battery then
      emitted = {}
      alerts[#alerts + 1] = "unplugged"
    else
      alerts[#alerts + 1] = "plugged"
    end
  end
  previous_on_battery = reading.on_battery
  if not reading.on_battery then
    return alerts
  end
  local crossed
  for _, candidate in ipairs {
    { options.warning_percent, "warning" },
    { options.persistent_percent, "persistent" },
    { options.critical_percent, "critical" },
  } do
    if reading.percentage <= candidate[1] and not emitted[candidate[1]] then
      emitted[candidate[1]] = true
      crossed = candidate[2]
    end
  end
  if crossed ~= nil then
    alerts[#alerts + 1] = crossed
  end
  return alerts
end

local function update(reading)
  if not reading.present or reading.percentage ~= reading.percentage or
    math.abs(reading.percentage) == math.huge then
    return
  end
  local snapshot = snapshot_for(reading)
  local immediate = last_snapshot == nil or snapshot.percent_text ~= last_snapshot.percent_text or
    snapshot.semantic_state ~= last_snapshot.semantic_state or snapshot.state_text ~= last_snapshot.state_text
  local now = GLib.get_monotonic_time() / 1000000.0
  if immediate or now - last_publish >= 30.0 then
    gisland.data(snapshot)
    last_snapshot = snapshot
    last_publish = now
  end
  local alerts = observe(reading)
  local saved, message = pcall(save_state)
  if not saved then
    gisland.log("error", "gisland-battery: cannot save battery state: " .. tostring(message))
  end
  for _, alert in ipairs(alerts) do
    publish_alert(alert, snapshot)
  end
end

local function property(proxy, name, default)
  local value = proxy:get_cached_property(name)
  if value == nil then
    return default
  end
  return value.value
end

local state_names = {
  [1] = "charging",
  [2] = "discharging",
  [3] = "empty",
  [4] = "fully-charged",
  [5] = "pending-charge",
  [6] = "pending-discharge",
}

local function emit_reading()
  if stopped or root_proxy == nil or device_proxy == nil then
    return
  end
  local state = state_names[math.tointeger(property(device_proxy, "State", 0))] or "unknown"
  update {
    percentage = tonumber(property(device_proxy, "Percentage", 0 / 0)),
    on_battery = not not property(root_proxy, "OnBattery", state == "discharging"),
    state = state,
    time_to_empty = math.tointeger(property(device_proxy, "TimeToEmpty", 0)),
    time_to_full = math.tointeger(property(device_proxy, "TimeToFull", 0)),
    energy_rate = tonumber(property(device_proxy, "EnergyRate", 0.0)),
    energy_full = tonumber(property(device_proxy, "EnergyFull", 0.0)),
    energy_full_design = tonumber(property(device_proxy, "EnergyFullDesign", 0.0)),
    present = not not property(device_proxy, "IsPresent", false),
  }
end

local function start_source()
  root_proxy = Gio.DBusProxy.new_for_bus_sync(
    Gio.BusType.SYSTEM,
    Gio.DBusProxyFlags.NONE,
    nil,
    "org.freedesktop.UPower",
    "/org/freedesktop/UPower",
    "org.freedesktop.UPower",
    nil
  )
  device_proxy = Gio.DBusProxy.new_for_bus_sync(
    Gio.BusType.SYSTEM,
    Gio.DBusProxyFlags.NONE,
    nil,
    "org.freedesktop.UPower",
    "/org/freedesktop/UPower/devices/DisplayDevice",
    "org.freedesktop.UPower.Device",
    nil
  )
  root_proxy.on_g_properties_changed = emit_reading
  device_proxy.on_g_properties_changed = emit_reading
  emit_reading()
end

local function validate_integer(config, name)
  local value = config[name]
  if value == nil then
    return options[name]
  end
  if math.type(value) ~= "integer" then
    error(name .. " must be an integer")
  end
  return value
end

local function configure(config)
  local unknown = {}
  for name in pairs(config) do
    if options[name] == nil then
      unknown[#unknown + 1] = name
    end
  end
  table.sort(unknown)
  if #unknown > 0 then
    error("unknown battery option: " .. unknown[1])
  end
  for name in pairs(options) do
    options[name] = validate_integer(config, name)
  end
  if options.critical_percent <= 0 or options.critical_percent >= options.persistent_percent or
    options.persistent_percent >= options.warning_percent or options.warning_percent > 100 then
    error(
      "critical_percent, persistent_percent and warning_percent must satisfy " ..
        "0 < critical_percent < persistent_percent < warning_percent <= 100"
    )
  end
  if options.red_percent <= 0 or options.red_percent >= options.yellow_percent or
    options.yellow_percent > 100 then
    error(
      "red_percent and yellow_percent must satisfy " ..
        "0 < red_percent < yellow_percent <= 100"
    )
  end
  if options.preview_duration_ms < 0 or options.preview_duration_ms > 60000 then
    error("preview_duration_ms must be between 0 and 60000")
  end
end

return gisland.module {
  init = function(config)
    configure(config)
    load_state()
    local ok, message = pcall(start_source)
    if not ok then
      root_proxy = nil
      device_proxy = nil
      gisland.log("error", "gisland-battery: UPower unavailable: " .. tostring(message))
    end
  end,
  actions = {
    ["dismiss-alert"] = function()
      if active_alert == nil then
        return false
      end
      gisland.dismiss(active_alert)
      active_alert = nil
      return true
    end,
  },
  fallback_action = function()
    return false
  end,
  shutdown = function()
    stopped = true
    if root_proxy ~= nil then
      root_proxy.on_g_properties_changed = nil
    end
    if device_proxy ~= nil then
      device_proxy.on_g_properties_changed = nil
    end
    root_proxy = nil
    device_proxy = nil
  end,
}
