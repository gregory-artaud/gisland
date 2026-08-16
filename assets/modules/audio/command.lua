local command = {}

local capture_limit = 64 * 1024

local function shell_quote(value)
  return "'" .. value:gsub("'", "'\\''") .. "'"
end

local function is_executable(path)
  return os.execute("test -x " .. shell_quote(path)) == true
end

local function gislandctl_command()
  local host_bindir = os.getenv("GISLAND_LUA_HOST_BINDIR")
  if host_bindir ~= nil and host_bindir ~= "" then
    local candidate = host_bindir .. "/gislandctl"
    if is_executable(candidate) then
      return shell_quote(candidate)
    end
  end

  local home = os.getenv("HOME")
  if home ~= nil and home ~= "" then
    local candidate = home .. "/.local/bin/gislandctl"
    if is_executable(candidate) then
      return shell_quote(candidate)
    end
  end

  return "gislandctl"
end

local function execute(shell_command)
  local pipe, open_error = io.popen(shell_command, "r")
  if pipe == nil then
    return {
      ok = false,
      output = open_error or "failed to start command",
      status = -1,
      timed_out = false,
      reason = "start",
    }
  end

  local chunks = {}
  local captured = 0
  while true do
    local chunk = pipe:read(4096)
    if chunk == nil then
      break
    end
    if captured < capture_limit then
      local remaining = capture_limit - captured
      local retained = chunk:sub(1, remaining)
      chunks[#chunks + 1] = retained
      captured = captured + #retained
    end
  end

  local closed, close_reason, status = pipe:close()
  if closed then
    return {
      ok = true,
      output = table.concat(chunks),
      status = 0,
      timed_out = false,
      reason = "ok",
    }
  end

  status = close_reason == "exit" and status or -1
  local timed_out = status == 124 or status == 137
  return {
    ok = false,
    output = table.concat(chunks),
    status = status,
    timed_out = timed_out,
    reason = timed_out and "timeout" or "exit",
  }
end

local function require_integer(value, name)
  if math.type(value) ~= "integer" or value < 0 then
    error(name .. " must be a non-negative integer", 3)
  end
  return value
end

function command.get_sink_volume()
  return execute("LC_ALL=C timeout --kill-after=0.2s 2s pactl 'get-sink-volume' '@DEFAULT_SINK@' 2>&1")
end

function command.get_sink_mute()
  return execute("LC_ALL=C timeout --kill-after=0.2s 2s pactl 'get-sink-mute' '@DEFAULT_SINK@' 2>&1")
end

function command.set_sink_volume(value)
  value = require_integer(value, "volume")
  return execute(
    "LC_ALL=C timeout --kill-after=0.2s 2s pactl 'set-sink-volume' '@DEFAULT_SINK@' '"
      .. tostring(value)
      .. "%' 2>&1"
  )
end

function command.set_sink_mute(value)
  if value == true then
    value = 1
  elseif value == false then
    value = 0
  end
  if math.type(value) ~= "integer" or (value ~= 0 and value ~= 1) then
    error("mute must be a boolean, 0, or 1", 2)
  end
  return execute(
    "LC_ALL=C timeout --kill-after=0.2s 2s pactl 'set-sink-mute' '@DEFAULT_SINK@' '"
      .. tostring(value)
      .. "' 2>&1"
  )
end

function command.close()
  return execute(
    "timeout --kill-after=0.2s 0.5s " .. gislandctl_command() .. " 'close' 2>&1"
  )
end

return command
