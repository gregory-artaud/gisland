local lgi = require("lgi")

assert(lgi.GLib, "lgi does not provide GLib")
assert(lgi.Gio, "lgi does not provide Gio")
assert(lgi.require("Json", "1.0"), "lgi does not provide Json-GLib 1.0")
assert(lgi.require("GdkPixbuf", "2.0"), "lgi does not provide GdkPixbuf 2.0")
assert(lgi.require("Rsvg", "2.0"), "lgi does not provide Rsvg 2.0")
assert(lgi.require("Gtk", "3.0"), "lgi does not provide Gtk 3.0")
