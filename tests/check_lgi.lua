local lgi = require("lgi")

assert(lgi.GLib, "lgi does not provide GLib")
assert(lgi.Gio, "lgi does not provide Gio")
assert(lgi.require("Json", "1.0"), "lgi does not provide Json-GLib 1.0")
