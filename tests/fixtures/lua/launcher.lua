local ui = gisland.ui
local query = ""

local applications = {
  { id = "terminal", name = "Terminal" },
  { id = "browser", name = "Web Browser" },
}

local function publish()
  local children = {
    ui.text_input {
      input_id = "query",
      action_id = "query-changed",
      initial_value = "",
      placeholder = "Search applications",
      accessible_label = "Search applications",
    },
  }
  for _, application in ipairs(applications) do
    if query == "" or string.find(string.lower(application.name), string.lower(query), 1, true) then
      table.insert(children, ui.button {
        action_id = "launch:" .. application.id,
        accessible_label = "Launch " .. application.name,
        close_on_activate = true,
        content = ui.text { value = application.name, role = "body" },
      })
    end
  end
  gisland.publish {
    context_id = "launcher",
    priority = 100,
    activation_only = true,
    views = {
      compact = ui.text { value = "Apps", role = "compact-primary" },
      expanded = ui.column { gap = "small", children = children },
    },
    presentation = { expanded_style = "palette" },
  }
end

return gisland.module {
  init = function(_, metadata)
    assert(metadata.protocol_minor >= 10)
    publish()
  end,
  actions = {
    ["query-changed"] = function(value)
      assert(type(value) == "string")
      assert(#value <= 1024)
      query = value
      gisland.data { observed_query = value }
      publish()
      return true
    end,
  },
  fallback_action = function(action_id)
    assert(action_id == "launch:terminal" or action_id == "launch:browser")
    gisland.data { launched = action_id, query_at_launch = query }
    return true
  end,
}
