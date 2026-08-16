local ui = gisland.ui
local count = 0

local function publish()
  gisland.publish {
    context_id = "counter",
    priority = 10,
    views = {
      compact = ui.button {
        action_id = "increment",
        accessible_label = "Increment counter",
        content = ui.text { value = tostring(count), role = "compact-primary" },
      },
    },
  }
end

return gisland.module {
  init = publish,
  actions = {
    increment = function()
      count = count + 1
      publish()
      return true
    end,
  },
}
