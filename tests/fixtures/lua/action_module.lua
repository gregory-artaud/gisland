local function observed(value)
  if value == nil then
    return { kind = "nil" }
  end
  return { kind = type(value), value = value }
end

return gisland.module {
  actions = {
    accept = function(value, ...)
      assert(select("#", ...) == 0, "action received hidden arguments")
      gisland.data(observed(value))
      return true
    end,
    reject = function(_) return false end,
    ["reject-reason"] = function(_) return false, "not available" end,
    ["publish-then-accept"] = function(_)
      gisland.data { callback = "published" }
      gisland.publish {
        context_id = "action-context",
        priority = 1,
        views = {
          compact = gisland.ui.text { value = "Action", role = "body" },
        },
      }
      return true
    end,
    throws = function(_) error("action exploded") end,
    ["invalid-type"] = function(_) return "yes" end,
    ["invalid-extra"] = function(_) return true, "extra" end,
    ["invalid-diagnostic"] = function(_) return false, 42 end,
    ["long-diagnostic"] = function(_) return false, string.rep("x", 4097) end,
  },
}
