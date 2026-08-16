return gisland.module {
  every = "1s",
  init = function(_) end,
  update = function() end,
  actions = {
    refresh = function(_) return true end,
    ["set-value"] = function(_) return true end,
  },
  visibility = function(_) end,
  shutdown = function() end,
}
