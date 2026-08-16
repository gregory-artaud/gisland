local expected_visibility = { "hidden", "compact-active", "expanded-active" }
local visibility_index = 1
local shutdown_count = 0

return gisland.module {
  init = function(config)
    assert(config.answer == 42, "configuration was not converted")
  end,
  visibility = function(value)
    assert(value == expected_visibility[visibility_index], "visibility callback order changed")
    visibility_index = visibility_index + 1
  end,
  shutdown = function()
    shutdown_count = shutdown_count + 1
    assert(shutdown_count == 1, "shutdown called more than once")
    assert(visibility_index == 4, "shutdown ran before visibility callbacks")
  end,
}
