local Gio = require("lgi").Gio
local ui = gisland.ui
local commands
local expanded_active = false
local instance_id
local pending
local labels = { restart = "Redémarrer", shutdown = "Éteindre" }

local function button(action, label, icon, closes)
  return ui.button {
    action_id = action,
    accessible_label = label,
    close_on_activate = closes,
    content = ui.column { alignment = "center", gap = "action-content", children = {
      ui.icon { name = icon, accessible_label = label, role = "action-icon" },
      ui.text { value = label, role = "action-label" },
    } },
  }
end

local function publish()
  local expanded
  if pending then
    expanded = ui.column { alignment = "center", gap = "large", children = {
      ui.text { value = labels[pending] .. " ?", role = "title" },
      ui.text { value = "Les applications ouvertes seront fermées.", role = "muted" },
      ui.row { alignment = "center", gap = "large", children = {
        ui.button { action_id = "cancel", accessible_label = "Annuler",
          content = ui.text { value = "Annuler", role = "body" } },
        ui.button { action_id = "confirm", accessible_label = labels[pending], close_on_activate = true,
          content = ui.text { value = labels[pending], role = "body" } },
      } },
    } }
  else
    expanded = ui.row { alignment = "center", gap = "normal", children = {
      button("lock", "Verrouiller", "lock", true),
      button("restart", "Redémarrer", "restart", false),
      button("shutdown", "Éteindre", "power", false),
    } }
  end
  gisland.publish {
    context_id = "power-menu", priority = 100, activation_only = true,
    views = { expanded = expanded },
    presentation = pending and { escape_action_id = "cancel" } or { expanded_style = "action-menu" },
  }
end

local function run(argv)
  local ok, process, failure = pcall(Gio.Subprocess.new, argv, Gio.SubprocessFlags.STDOUT_SILENCE)
  if not ok or not process then
    gisland.log("error", "Unable to start command: " .. tostring(failure or process))
    return
  end
  process:wait_check_async(nil, function(candidate, result)
    local finished, success, error = pcall(function() return candidate:wait_check_finish(result) end)
    if not finished or not success then
      gisland.log("error", "Command failed: " .. tostring(error or success))
    end
  end)
end

local function execute(action)
  local argv = commands[action]
  if not argv then return false, "Invalid command configuration" end
  pending = nil
  publish()
  gisland.defer(function() run(argv) end)
  return true
end

return gisland.module {
  init = function(config, metadata)
    instance_id = metadata.instance_id
    commands = {
      lock = config.lock_command or { "i3lock" },
      restart = config.restart_command or { "reboot" },
      shutdown = config.shutdown_command or { "poweroff" },
    }
    for action, argv in pairs(commands) do
      local valid = type(argv) == "table" and #argv > 0
      if valid then
        for _, argument in ipairs(argv) do
          if type(argument) ~= "string" or argument == "" or argument:find("%z") then
            valid = false
            break
          end
        end
      end
      if not valid then
        commands[action] = nil
        gisland.log("error", "Invalid " .. action .. "_command: expected a non-empty argv array")
      end
    end
    publish()
  end,
  visibility = function(state)
    expanded_active = state == "expanded-active"
    if not expanded_active and pending then pending = nil publish() end
  end,
  actions = {
    toggle = function()
      local directory = os.getenv("GISLAND_LUA_HOST_BINDIR")
      if not directory or directory == "" then return false, "Lua host directory unavailable" end
      local argv = { directory .. "/gislandctl", "close" }
      if not expanded_active then
        pending = nil
        publish()
        argv = { directory .. "/gislandctl", "activate-open", instance_id }
      end
      gisland.defer(function() run(argv) end)
      return true
    end,
    lock = function() return execute("lock") end,
    restart = function() pending = "restart" publish() return true end,
    shutdown = function() pending = "shutdown" publish() return true end,
    cancel = function() pending = nil publish() return true end,
    confirm = function()
      if not pending then return false, "No confirmation pending" end
      return execute(pending)
    end,
  },
}
