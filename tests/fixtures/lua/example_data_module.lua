return gisland.module {
  every = "1s",

  update = function()
    return {
      time = os.date("%H:%M"),
      date = os.date("%A %d"),
    }
  end,
}
