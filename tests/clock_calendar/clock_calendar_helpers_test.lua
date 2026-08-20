local GLib = require("lgi").GLib
local entry = assert(arg[1], "clock-calendar entry is required")
assert(os.getenv("GISLAND_CLOCK_CALENDAR_HELPER_TEST") == "1")
local calendar = assert(dofile(entry))

local now = assert(GLib.DateTime.new_utc(2026, 8, 3, 14, 35, 42.25))
assert(calendar.next_minute_delay_ms(now) == 17750)

local boundary = assert(GLib.DateTime.new_utc(2026, 8, 3, 14, 36, 0))
assert(calendar.next_minute_delay_ms(boundary) == 60000)

assert(calendar.is_leap_year(2000))
assert(not calendar.is_leap_year(1900))
assert(not calendar.is_leap_year(2100))
assert(calendar.is_leap_year(2400))
assert(calendar.days_in_month(2024, 2) == 29)
assert(calendar.days_in_month(2026, 2) == 28)

local expected_month_lengths = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
for month, expected in ipairs(expected_month_lengths) do
  assert(calendar.days_in_month(2026, month) == expected)
end

local civil_dates = {
  { 0, 12, 31 },
  { 1, 1, 1 },
  { 1900, 3, 1 },
  { 2000, 2, 29 },
  { 9999, 12, 31 },
  { 10000, 1, 1 },
}
for _, civil in ipairs(civil_dates) do
  local ordinal = calendar.civil_to_ordinal(table.unpack(civil))
  local year, month, day = calendar.ordinal_to_civil(ordinal)
  assert(year == civil[1] and month == civil[2] and day == civil[3])
end
assert(calendar.weekday(1, 1, 1) == 1)
assert(calendar.weekday(1970, 1, 1) == 4)

local year, month = calendar.shift_month(2026, 1, -1)
assert(year == 2025 and month == 12)
year, month = calendar.shift_month(2026, 12, 1)
assert(year == 2027 and month == 1)
assert(calendar.shift_month(1, 1, -1) == nil)
assert(calendar.shift_month(9999, 12, 1) == nil)

local utc = assert(GLib.TimeZone.new_identifier("UTC"))
local january_year_one = calendar.snapshot(GLib, utc, now, 1, 1, "sunday")
assert(january_year_one.month_label == "January 1")
assert(january_year_one.weeks[1][1].label == "31")
assert(january_year_one.weeks[1][1].role == "muted")
assert(january_year_one.weeks[1][2].label == "01")
assert(january_year_one.weeks[1][2].role == "body")

local december_year_9999 = calendar.snapshot(GLib, utc, now, 9999, 12, "monday")
assert(december_year_9999.month_label == "December 9999")
assert(december_year_9999.weeks[1][1].label == "29")
assert(december_year_9999.weeks[6][7].label == "09")
assert(december_year_9999.weeks[6][7].role == "muted")

local apia = assert(GLib.TimeZone.new_identifier("Pacific/Apia"))
local apia_december = calendar.snapshot(GLib, apia, now, 2011, 12, "monday")
assert(apia_december.weeks[5][5].label == "30")
assert(apia_december.weeks[5][5].role == "body")
local december_thirties = 0
for _, week in ipairs(apia_december.weeks) do
  for _, day in ipairs(week) do
    if day.label == "30" and day.role == "body" then
      december_thirties = december_thirties + 1
    end
  end
end
assert(december_thirties == 1)
