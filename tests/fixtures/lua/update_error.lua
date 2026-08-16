return gisland.module {
  every = "1ms",
  update = function()
    error("update failed")
  end,
}
