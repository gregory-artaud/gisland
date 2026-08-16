local sequence = 0

local function emit()
  sequence = sequence + 1
  gisland.data { sequence = sequence }
end

return gisland.module {
  every = "20ms",
  init = function()
    emit()
    gisland.defer(emit)
    gisland.after("10ms", emit)
    gisland.after("10ms", emit)
  end,
  update = function()
    sequence = sequence + 1
    return { sequence = sequence }
  end,
}
