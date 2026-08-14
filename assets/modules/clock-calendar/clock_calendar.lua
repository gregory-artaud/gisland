local GLib = require("lgi").GLib

local calendar = {}

local function integer(value)
  return math.tointeger(value) or math.floor(value)
end

function calendar.next_minute_delay_ms(now)
  local elapsed = integer(now:get_second()) * 1000 + integer(now:get_microsecond() / 1000)
  return 60000 - elapsed
end

function calendar.is_leap_year(year)
  return year % 4 == 0 and (year % 100 ~= 0 or year % 400 == 0)
end

function calendar.days_in_month(year, month)
  if month == 2 then
    return calendar.is_leap_year(year) and 29 or 28
  end
  if month == 4 or month == 6 or month == 9 or month == 11 then
    return 30
  end
  return 31
end

function calendar.civil_to_ordinal(year, month, day)
  local adjusted_year = month <= 2 and year - 1 or year
  local era = adjusted_year // 400
  local year_of_era = adjusted_year - era * 400
  local shifted_month = month + (month > 2 and -3 or 9)
  local day_of_year = (153 * shifted_month + 2) // 5 + day - 1
  local day_of_era = year_of_era * 365 + year_of_era // 4 - year_of_era // 100 + day_of_year
  return era * 146097 + day_of_era - 719468
end

function calendar.ordinal_to_civil(ordinal)
  local shifted = ordinal + 719468
  local era = shifted // 146097
  local day_of_era = shifted - era * 146097
  local year_of_era = (day_of_era - day_of_era // 1460 + day_of_era // 36524 -
    day_of_era // 146096) // 365
  local year = year_of_era + era * 400
  local day_of_year = day_of_era -
    (365 * year_of_era + year_of_era // 4 - year_of_era // 100)
  local shifted_month = (5 * day_of_year + 2) // 153
  local day = day_of_year - (153 * shifted_month + 2) // 5 + 1
  local month = shifted_month + (shifted_month < 10 and 3 or -9)
  year = year + (month <= 2 and 1 or 0)
  return year, month, day
end

function calendar.weekday(year, month, day)
  return (calendar.civil_to_ordinal(year, month, day) + 4) % 7
end

function calendar.shift_month(year, month, delta)
  local index = (year * 12) + (month - 1) + delta
  local shifted_year = math.floor(index / 12)
  local shifted_month = (index % 12) + 1
  if shifted_year < 1 or shifted_year > 9999 then
    return nil
  end
  return shifted_year, shifted_month
end

function calendar.snapshot(GLib, zone, now, displayed_year, displayed_month, week_start)
  local weekdays = {}
  local weeks = {}
  local calendar_columns = {}
  local label_start = calendar.civil_to_ordinal(2024, 1, week_start == "sunday" and 7 or 1)
  for index = 0, 6 do
    local year, month, day = calendar.ordinal_to_civil(label_start + index)
    local label_date = assert(GLib.DateTime.new(zone, year, month, day, 12, 0, 0))
    weekdays[#weekdays + 1] = label_date:format("%a")
  end

  local first = assert(GLib.DateTime.new(zone, displayed_year, displayed_month, 1, 12, 0, 0))
  local weekday = calendar.weekday(displayed_year, displayed_month, 1)
  local offset = week_start == "monday" and (weekday + 6) % 7 or weekday
  local grid_start = calendar.civil_to_ordinal(displayed_year, displayed_month, 1) - offset
  local current_year = integer(now:get_year())
  local current_month = integer(now:get_month())
  local current_day = integer(now:get_day_of_month())

  for week_index = 0, 5 do
    local week = {}
    for day_index = 0, 6 do
      local year, month, day =
        calendar.ordinal_to_civil(grid_start + (week_index * 7) + day_index)
      local in_month = year == displayed_year and month == displayed_month
      local role = "muted"
      if in_month and year == current_year and month == current_month and day == current_day then
        role = "accent"
      elseif in_month then
        role = "body"
      end
      week[#week + 1] = { label = string.format("%02d", day), role = role }
    end
    weeks[#weeks + 1] = week
  end

  for day_index = 1, 7 do
    local days = {}
    for week_index = 1, 6 do
      days[#days + 1] = weeks[week_index][day_index]
    end
    calendar_columns[#calendar_columns + 1] = {
      weekday = weekdays[day_index],
      days = days,
    }
  end

  return {
    time = now:format("%H:%M"),
    date_short = now:format("%a") .. " " .. tostring(current_day) .. " " .. now:format("%b"),
    month_label = first:format("%B") .. " " .. tostring(displayed_year),
    weekdays = weekdays,
    weeks = weeks,
    calendar_columns = calendar_columns,
  }
end

if os.getenv("GISLAND_CLOCK_CALENDAR_HELPER_TEST") == "1" then
  return calendar
end

local zone
local week_start = "monday"
local displayed_year
local displayed_month
local follows_current_month = true
local test_now_file = os.getenv("GISLAND_CLOCK_CALENDAR_TEST_NOW_FILE")

local function current_time()
  if test_now_file == nil then
    return assert(GLib.DateTime.new_now(zone))
  end
  local stream = assert(io.open(test_now_file, "r"))
  local value = stream:read("*a")
  stream:close()
  local seconds = tonumber(value)
  if seconds == nil or math.type(seconds) ~= "integer" then
    error("invalid injected clock fixture")
  end
  return assert(GLib.DateTime.new_from_unix_utc(seconds):to_timezone(zone))
end

local function synchronize_displayed_month(now)
  if follows_current_month then
    displayed_year = math.tointeger(now:get_year())
    displayed_month = math.tointeger(now:get_month())
  end
end

local function snapshot()
  local now = current_time()
  synchronize_displayed_month(now)
  return calendar.snapshot(GLib, zone, now, displayed_year, displayed_month, week_start)
end

local function publish()
  gisland.data(snapshot())
end

local function schedule_next_minute()
  local delay = calendar.next_minute_delay_ms(current_time())
  gisland.after(tostring(delay) .. "ms", function()
    publish()
    schedule_next_minute()
  end)
end

local function navigate(delta)
  local now = current_time()
  synchronize_displayed_month(now)
  local year, month = calendar.shift_month(displayed_year, displayed_month, delta)
  if year == nil then
    return false
  end
  displayed_year = year
  displayed_month = month
  follows_current_month = false
  gisland.defer(publish)
  return true
end

local function today()
  local now = current_time()
  displayed_year = math.tointeger(now:get_year())
  displayed_month = math.tointeger(now:get_month())
  follows_current_month = true
  gisland.defer(publish)
  return true
end

return gisland.module {
  init = function(config, metadata)
    for name in pairs(config) do
      if name ~= "locale" and name ~= "timezone" and name ~= "week_start" then
        error("unknown clock-calendar option: " .. name)
      end
    end
    local locale = config.locale or metadata.locale
    local timezone_name = config.timezone or metadata.timezone
    week_start = config.week_start or "monday"
    if locale == "" then
      error("locale must not be empty")
    end
    if timezone_name == "" then
      error("timezone must not be empty")
    end
    if week_start ~= "monday" and week_start ~= "sunday" then
      error("week_start must be monday or sunday")
    end
    if os.setlocale(locale, "time") == nil then
      error("invalid locale: " .. locale)
    end
    zone = GLib.TimeZone.new_identifier(timezone_name)
    if zone == nil then
      error("invalid timezone: " .. timezone_name)
    end
    local now = current_time()
    displayed_year = math.tointeger(now:get_year())
    displayed_month = math.tointeger(now:get_month())
    publish()
    schedule_next_minute()
  end,
  actions = {
    ["previous-month"] = function()
      return navigate(-1)
    end,
    ["next-month"] = function()
      return navigate(1)
    end,
    ["today"] = today,
  },
  fallback_action = function()
    return false
  end,
  visibility = function()
    if test_now_file ~= nil then
      publish()
    end
  end,
}
