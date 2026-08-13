local format = "%H:%M:%S"

return gisland.module {
  every = "1s",

  init = function(config)
    format = config.format
  end,

  update = function()
    return {
      time = os.date(format),
      date = os.date("%Y-%m-%d"),
    }
  end,
}
