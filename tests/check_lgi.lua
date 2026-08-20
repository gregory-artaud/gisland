local lgi = require("lgi")

local staged_prefix = os.getenv("GISLAND_LGI_TEST_PREFIX")
if staged_prefix then
  for entry in package.path:gmatch("[^;]+") do
    assert(entry:sub(1, #staged_prefix + 1) == staged_prefix .. "/",
           "Lua path escaped staged lgi prefix: " .. entry)
  end
  for entry in package.cpath:gmatch("[^;]+") do
    assert(entry:sub(1, #staged_prefix + 1) == staged_prefix .. "/",
           "Lua native path escaped staged lgi prefix: " .. entry)
  end
  local native = assert(package.searchpath("lgi.corelgilua51", package.cpath),
                        "staged lgi native module is unavailable")
  assert(native:sub(1, #staged_prefix + 1) == staged_prefix .. "/",
         "lgi native module escaped staged prefix: " .. native)
end

assert(lgi.GLib, "lgi does not provide GLib")
assert(lgi.Gio, "lgi does not provide Gio")
assert(lgi.require("GioUnix", "2.0"), "lgi does not provide GioUnix 2.0")
assert(lgi.require("Json", "1.0"), "lgi does not provide Json-GLib 1.0")
assert(lgi.require("GdkPixbuf", "2.0"), "lgi does not provide GdkPixbuf 2.0")
assert(lgi.require("Rsvg", "2.0"), "lgi does not provide Rsvg 2.0")
assert(lgi.require("Gtk", "3.0"), "lgi does not provide Gtk 3.0")
