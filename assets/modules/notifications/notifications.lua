local lgi = require("lgi")
local Gio = lgi.Gio
local GLib = lgi.GLib
local GdkPixbuf = lgi.require("GdkPixbuf", "2.0")
local Json = lgi.require("Json", "1.0")
local Gtk = lgi.require("Gtk", "3.0")

local BUS_NAME = "org.freedesktop.Notifications"
local OBJECT_PATH = "/org/freedesktop/Notifications"
local INTERFACE = BUS_NAME
local UINT32_MAX = 0xffffffff
local MAX_ITEMS = 128
local MAX_TEXT_BYTES = 16 * 1024
local MAX_SPAN_BYTES = 4096
local MAX_BODY_INPUT_BYTES = 64 * 1024
local MAX_APP_NAME_BYTES = 1024
local MAX_SUMMARY_BYTES = 4096
local MAX_ICON_BYTES = 4096
local MAX_ACTIONS = 32
local MAX_ACTION_KEY_BYTES = 1024
local MAX_ACTION_LABEL_BYTES = 4096
local MAX_HINTS = 64
local MAX_HINT_KEY_BYTES = 256
local MAX_MARKUP_DEPTH = 16
local MAX_MARKUP_ELEMENTS = 128
local MAX_IMAGE_DIMENSION = 512
local MAX_IMAGE_INPUT_BYTES = 4 * 1024 * 1024
local MAX_IMAGE_FILE_BYTES = 8 * 1024 * 1024
local MAX_RESOURCE_COUNT = 16
local MAX_RESOURCE_BYTES = 4 * 1024 * 1024
local MAX_HISTORY_LIMIT = 1000
local MAX_HISTORY_TEXT_BYTES = 4096
local MAX_HISTORY_FILE_BYTES = 1024 + MAX_HISTORY_LIMIT * (18 * MAX_HISTORY_TEXT_BYTES + 256)
local HISTORY_OPEN_TIMEOUT_MS = 2000
local HISTORY_INACTIVITY_TIMEOUT_MS = 8000

local introspection_xml = [[
<node>
  <interface name="org.freedesktop.Notifications">
    <method name="GetCapabilities"><arg direction="out" name="capabilities" type="as"/></method>
    <method name="Notify">
      <arg direction="in" name="app_name" type="s"/><arg direction="in" name="replaces_id" type="u"/>
      <arg direction="in" name="app_icon" type="s"/><arg direction="in" name="summary" type="s"/>
      <arg direction="in" name="body" type="s"/><arg direction="in" name="actions" type="as"/>
      <arg direction="in" name="hints" type="a{sv}"/><arg direction="in" name="expire_timeout" type="i"/>
      <arg direction="out" name="id" type="u"/>
    </method>
    <method name="CloseNotification"><arg direction="in" name="id" type="u"/></method>
    <method name="GetServerInformation">
      <arg direction="out" name="name" type="s"/><arg direction="out" name="vendor" type="s"/>
      <arg direction="out" name="version" type="s"/><arg direction="out" name="spec_version" type="s"/>
    </method>
    <signal name="NotificationClosed"><arg name="id" type="u"/><arg name="reason" type="u"/></signal>
    <signal name="ActionInvoked"><arg name="id" type="u"/><arg name="action_key" type="s"/></signal>
  </interface>
</node>
]]

local capabilities = {
  "actions", "body", "body-markup", "body-hyperlinks", "body-images", "icon-static", "persistence",
}
local options = { reveal_duration_ms = 1000, history_limit = 100, history_visible_limit = 5 }
local connection
local registration_id
local name_subscription
local stopped = false
local next_id = 1
local live = {}
local live_count = 0
local routes = {}
local timers = {}
local history = {}
local next_sequence = 1
local history_sequences = {}
local history_visible_count = 0
local history_was_expanded = false
local latest_visibility = "hidden"
local history_session_id = 0
local history_hidden = {}
local history_open_timer
local history_inactivity_timer
local history_save_generation = 0
local history_writer_generation = 0
local history_save_active
local history_save_pending
local history_save_latest
local history_save_delay_source
local history_save_cancellable

local function array(values)
  local result = gisland.array()
  for index, value in ipairs(values) do
    result[index] = value
  end
  return result
end

local function bounded_utf8(value, maximum)
  if #value <= maximum then
    return value
  end
  local result = value:sub(1, maximum)
  while not utf8.len(result) do
    result = result:sub(1, -2)
  end
  return result
end

local function invalid(message)
  error({ dbus_error = "org.freedesktop.DBus.Error.InvalidArgs", message = message }, 0)
end

local function require_string(value, maximum, name)
  if type(value) ~= "string" then invalid(name .. " must be a string") end
  if #value > maximum then invalid(name .. " exceeds the byte limit") end
  return value
end

local function variant_value(value)
  if type(value) == "userdata" then
    return value.value
  end
  return value
end

local function variant_array(value)
  value = variant_value(value)
  local result = {}
  for index = 1, #value do
    result[index] = variant_value(value[index])
  end
  return result
end

local function plain(value)
  local items = {}
  value = bounded_utf8(value, MAX_TEXT_BYTES)
  while value ~= "" and #items < MAX_ITEMS do
    local span = bounded_utf8(value, MAX_SPAN_BYTES)
    items[#items + 1] = { type = "text", value = span }
    value = value:sub(#span + 1)
  end
  return { items = items, links = {}, images = {} }
end

local function parse_body(value)
  local lowered = value:lower()
  if lowered:find("<!doctype", 1, true) or lowered:find("<!entity", 1, true) then
    return plain(value)
  end
  local document = { tag = "root", attr = {} }
  local stack = { document }
  local element_count = 0
  local parser = GLib.MarkupParser {
    start_element = function(_, tag, attributes)
      element_count = element_count + 1
      if element_count > MAX_MARKUP_ELEMENTS then error("body markup has too many elements") end
      if #stack >= MAX_MARKUP_DEPTH + 1 then error("body markup is too deep") end
      stack[#stack + 1] = { tag = tag:lower(), attr = attributes }
    end,
    text = function(_, text)
      stack[#stack][#stack[#stack] + 1] = text
    end,
    end_element = function()
      local child = stack[#stack]
      stack[#stack] = nil
      stack[#stack][#stack[#stack] + 1] = child
    end,
  }
  local context = GLib.MarkupParseContext(parser, "TREAT_CDATA_AS_TEXT")
  local ok = pcall(function()
    assert(context:parse("<root>" .. value .. "</root>"))
    assert(context:end_parse())
  end)
  if not ok or document[1] == nil then
    return plain(value)
  end

  local parsed = { items = {}, links = {}, images = {} }
  local text_bytes = 0
  local link_count = 0
  local image_count = 0
  local function append_text(text, emphasis, link_id)
    if text == "" or #parsed.items >= MAX_ITEMS or text_bytes >= MAX_TEXT_BYTES then
      return
    end
    text = bounded_utf8(text, MAX_TEXT_BYTES - text_bytes)
    while text ~= "" and #parsed.items < MAX_ITEMS do
      local span = bounded_utf8(text, MAX_SPAN_BYTES)
      text = text:sub(#span + 1)
      text_bytes = text_bytes + #span
      local item
      if link_id then
        item = {
          type = "link", value = span, emphasis = array(emphasis), action_id = link_id,
          accessible_label = bounded_utf8(span, MAX_ACTION_LABEL_BYTES),
        }
      else
        item = { type = "text", value = span }
        if #emphasis > 0 then
          item.emphasis = array(emphasis)
        end
      end
      parsed.items[#parsed.items + 1] = item
    end
  end
  local function walk(element, emphasis, link_id)
    for _, child in ipairs(element) do
      if type(child) == "string" then
        append_text(child, emphasis, link_id)
      else
        local child_emphasis = { table.unpack(emphasis) }
        local child_link = link_id
        if child.tag == "b" or child.tag == "i" or child.tag == "u" then
          local named = ({ b = "bold", i = "italic", u = "underline" })[child.tag]
          local present = false
          for _, value in ipairs(child_emphasis) do
            present = present or value == named
          end
          if not present then child_emphasis[#child_emphasis + 1] = named end
        elseif child.tag == "a" and link_count < 32 then
          local href = bounded_utf8(child.attr.href or "", MAX_ICON_BYTES)
          if href ~= "" then
            child_link = "link-" .. tostring(link_count)
            link_count = link_count + 1
            parsed.links[child_link] = href
          end
        elseif child.tag == "img" and #parsed.items < MAX_ITEMS then
          local source = bounded_utf8(child.attr.src or "", MAX_ICON_BYTES)
          if source ~= "" then
            local resource_id = "inline-" .. tostring(image_count)
            image_count = image_count + 1
            local label = bounded_utf8(child.attr.alt or "", MAX_ACTION_LABEL_BYTES)
            parsed.images[resource_id] = { source, label }
            parsed.items[#parsed.items + 1] = {
              type = "inline_image", resource_id = resource_id,
              role = "notification-inline-image", accessible_label = label,
            }
          end
        end
        if child.tag ~= "img" then walk(child, child_emphasis, child_link) end
      end
    end
  end
  walk(document[1], {}, nil)
  return parsed
end

local function normalize_raw_image(raw)
  raw = variant_value(raw)
  if type(raw) ~= "table" and type(raw) ~= "userdata" then error("image data must be a seven-field tuple") end
  local width = math.tointeger(raw[1])
  local height = math.tointeger(raw[2])
  local rowstride = math.tointeger(raw[3])
  local has_alpha = raw[4]
  local bits = math.tointeger(raw[5])
  local channels = math.tointeger(raw[6])
  local source = raw[7]
  if not width or not height or not rowstride or type(has_alpha) ~= "boolean" or not bits or not channels or
    type(source) ~= "string" then error("image data has invalid field types") end
  if width <= 0 or height <= 0 or width > 16384 or height > 16384 then error("image dimensions are invalid") end
  if bits ~= 8 or (channels ~= 3 and channels ~= 4) or has_alpha ~= (channels == 4) then
    error("only RGB8 and RGBA8 image data are supported")
  end
  if width > math.maxinteger // channels or rowstride < width * channels then
    error("image rowstride is too small")
  end
  if rowstride > MAX_IMAGE_INPUT_BYTES or height > MAX_IMAGE_INPUT_BYTES // rowstride then
    error("image data exceeds the byte limit")
  end
  if #source ~= rowstride * height or #source > MAX_IMAGE_INPUT_BYTES then
    error("image byte count does not match its rowstride")
  end
  local target_width, target_height = width, height
  if width > MAX_IMAGE_DIMENSION or height > MAX_IMAGE_DIMENSION then
    local scale = math.min(MAX_IMAGE_DIMENSION / width, MAX_IMAGE_DIMENSION / height)
    target_width = math.max(1, math.floor(width * scale))
    target_height = math.max(1, math.floor(height * scale))
  end
  local pixels = {}
  for target_y = 0, target_height - 1 do
    local source_y = math.min(height - 1, target_y * height // target_height)
    local row = {}
    for target_x = 0, target_width - 1 do
      local source_x = math.min(width - 1, target_x * width // target_width)
      local offset = source_y * rowstride + source_x * channels + 1
      row[#row + 1] = string.char(source:byte(offset), source:byte(offset + 1),
        source:byte(offset + 2), has_alpha and source:byte(offset + 3) or 255)
    end
    pixels[#pixels + 1] = table.concat(row)
  end
  return { width = target_width, height = target_height, pixels = table.concat(pixels) }
end

local function percent_decode(value)
  local offset = 1
  while true do
    local marker = value:find("%", offset, true)
    if not marker then break end
    if not value:sub(marker + 1, marker + 2):match("^%x%x$") then
      error("file URI has invalid escaping")
    end
    offset = marker + 3
  end
  local decoded = value:gsub("%%(%x%x)", function(hex) return string.char(tonumber(hex, 16)) end)
  if decoded:find("\0", 1, true) then error("file path contains NUL") end
  return decoded
end

local function local_path(value)
  if type(value) ~= "string" or value == "" or value:find("\0", 1, true) then
    error("image path is invalid")
  end
  if value:match("^file:") then
    if value:find("?", 1, true) or value:find("#", 1, true) then
      error("file URI must not contain a query or fragment")
    end
    local authority, path = value:match("^file://([^/]*)(/.*)$")
    if authority == nil or (authority ~= "" and authority ~= "localhost") then
      error("non-local file URI is unsupported")
    end
    return percent_decode(path)
  end
  if value:match("^[%a][%w+.-]*:") then error("remote image sources are unsupported") end
  return value
end

local function regular_file(path, maximum)
  local file = Gio.File.new_for_path(path)
  local info = file:query_info("standard::type,standard::size", Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, nil)
  if tostring(info:get_file_type()) ~= "REGULAR" then error("image is not a regular file") end
  local size = math.tointeger(info:get_size())
  if not size or size < 0 or size > maximum then error("image file exceeds the byte limit") end
  return size
end

local function validate_svg(path, size)
  local stream = assert(Gio.File.new_for_path(path):read(nil))
  local bytes = assert(stream:read_bytes(size + 1, nil)):get_data()
  stream:close(nil)
  if #bytes > size then error("image file changed while reading") end
  local lowered = bytes:lower()
  if lowered:find("<!doctype", 1, true) or lowered:find("<!entity", 1, true) then
    error("unsafe SVG declaration")
  end
  local elements = 0
  for _ in bytes:gmatch("<[%a_:]") do
    elements = elements + 1
    if elements > 4096 then error("SVG has too many elements") end
  end
end

local function looks_like_svg(path, size)
  local stream = assert(Gio.File.new_for_path(path):read(nil))
  local prefix = assert(stream:read_bytes(math.min(size, 4096), nil)):get_data():lower()
  stream:close(nil)
  return prefix:find("<svg", 1, true) ~= nil
end

local function load_image_file(value)
  local path = local_path(value)
  local size = regular_file(path, MAX_IMAGE_FILE_BYTES)
  if path:lower():match("%.svgz$") then error("compressed SVG images are unsupported") end
  if path:lower():match("%.svg$") or looks_like_svg(path, size) then validate_svg(path, size) end
  local _, source_width, source_height = GdkPixbuf.Pixbuf.get_file_info(path)
  if not source_width or not source_height or source_width <= 0 or source_height <= 0 or
      source_width > 1000000 or source_height > 1000000 then
    error("image dimensions are invalid")
  end
  local pixbuf
  if source_width > MAX_IMAGE_DIMENSION or source_height > MAX_IMAGE_DIMENSION then
    pixbuf = GdkPixbuf.Pixbuf.new_from_file_at_scale(
      path, MAX_IMAGE_DIMENSION, MAX_IMAGE_DIMENSION, true)
  else
    pixbuf = GdkPixbuf.Pixbuf.new_from_file(path)
  end
  local pixels = pixbuf:read_pixel_bytes():get_data()
  return normalize_raw_image {
    pixbuf:get_width(), pixbuf:get_height(), pixbuf:get_rowstride(), pixbuf:get_has_alpha(),
    pixbuf:get_bits_per_sample(), pixbuf:get_n_channels(), pixels,
  }
end

local function load_icon_name(name)
  local theme = Gtk.IconTheme.get_default() or Gtk.IconTheme.new()
  local icon = theme:lookup_icon(name, MAX_IMAGE_DIMENSION, Gtk.IconLookupFlags.FORCE_SVG)
  if not icon or not icon:get_filename() then error("icon was not found: " .. name) end
  return load_image_file(icon:get_filename())
end

local function desktop_entry_icon(name)
  if name == "" or #name > 255 or name:find("/", 1, true) or name:find("\0", 1, true) then
    error("desktop entry ID is invalid")
  end
  local filename = name:match("%.desktop$") and name or name .. ".desktop"
  local roots = { os.getenv("XDG_DATA_HOME") or ((os.getenv("HOME") or "") .. "/.local/share") }
  for root in (os.getenv("XDG_DATA_DIRS") or "/usr/local/share:/usr/share"):gmatch("[^:]+") do
    roots[#roots + 1] = root
  end
  for _, root in ipairs(roots) do
    local directory = root .. "/applications"
    local candidate = directory .. "/" .. filename
    if not GLib.file_test(candidate, "IS_REGULAR") then
      local dir = Gio.File.new_for_path(directory)
      local ok, enumerator = pcall(function()
        return dir:enumerate_children("standard::name,standard::type", Gio.FileQueryInfoFlags.NONE, nil)
      end)
      if ok then
        while true do
          local info = enumerator:next_file(nil)
          if not info then break end
          if info:get_name():lower() == filename:lower() then candidate = directory .. "/" .. info:get_name() break end
        end
        enumerator:close(nil)
      end
    end
    if GLib.file_test(candidate, "IS_REGULAR") then
      regular_file(candidate, 1024 * 1024)
      local contents = assert(GLib.file_get_contents(candidate))
      local in_entry = false
      for line in contents:gmatch("[^\r\n]+") do
        if line:match("^%[") then in_entry = line == "[Desktop Entry]" end
        local icon = in_entry and line:match("^Icon%s*=%s*(.-)%s*$")
        if icon and icon ~= "" then
          if icon:find("/", 1, true) or icon:match("^file:") then return load_image_file(icon) end
          return load_icon_name(icon)
        end
      end
      error("desktop entry has no icon")
    end
  end
  error("desktop entry was not found")
end

local function resolve_app_image(hints, app_icon)
  for _, key in ipairs { "image-data", "image_data", "icon_data" } do
    local value = hints[key]
    if value ~= nil then
      local ok, image = pcall(normalize_raw_image, value)
      if ok then return image end
      break
    end
  end
  for _, key in ipairs { "image-path", "image_path" } do
    local value = hints[key]
    if type(value) == "string" and value ~= "" then
      local ok, image = pcall(load_image_file, value)
      if ok then return image end
      break
    end
  end
  if app_icon ~= "" then
    local ok, image = pcall(function()
      if app_icon:find("/", 1, true) or app_icon:match("^file:") then return load_image_file(app_icon) end
      return load_icon_name(app_icon)
    end)
    if ok then return image end
  end
  if type(hints["desktop-entry"]) == "string" and hints["desktop-entry"] ~= "" then
    local ok, image = pcall(desktop_entry_icon, hints["desktop-entry"])
    if ok then return image end
  end
  return nil
end

local function resource(id, image)
  return { id = id, format = "rgba8", width = image.width, height = image.height,
    data = GLib.base64_encode(image.pixels) }
end

local function text(value, role)
  return { type = "text", value = value, role = role }
end

local function notification_publication(notification, app_image)
  local parsed = parse_body(notification.body)
  local prefix = "notification-" .. tostring(notification.id) .. ":"
  local routing = { [prefix .. "close"] = { "close", "close" } }
  for id, uri in pairs(parsed.links) do routing[prefix .. id] = { "uri", uri } end
  local resources = {}
  local resource_bytes = 0
  local app_resource
  if app_image and #app_image.pixels <= MAX_RESOURCE_BYTES then
    app_resource = resource("app-image", app_image)
    resources[#resources + 1] = app_resource
    resource_bytes = #app_image.pixels
  end
  local header = {}
  if app_resource then
    header[#header + 1] = { type = "image", resource_id = "app-image", role = "notification-header-icon",
      accessible_label = notification.app_name ~= "" and notification.app_name or "Application" }
  end
  local titles = {}
  if notification.app_name ~= "" then titles[#titles + 1] = text(notification.app_name:upper(), "caption") end
  if notification.summary ~= "" then titles[#titles + 1] = text(notification.summary, "body") end
  if #titles > 0 then
    header[#header + 1] = { type = "column", alignment = "start", gap = "xsmall", children = array(titles) }
  end
  header[#header + 1] = { type = "spacer", flexible = true }
  header[#header + 1] = { type = "action_region", action_id = prefix .. "close",
    accessible_label = "Close notification", content = { type = "icon", name = "close",
      accessible_label = "Close notification" } }
  local children = { { type = "row", gap = "small", children = array(header) } }
  local body_items = {}
  local accepting_resources = #resources < MAX_RESOURCE_COUNT and resource_bytes < MAX_RESOURCE_BYTES
  for _, item in ipairs(parsed.items) do
    if item.type == "inline_image" then
      if accepting_resources then
        local image_source = parsed.images[item.resource_id]
        local ok, image = pcall(load_image_file, image_source[1])
        if ok then
          if #image.pixels <= MAX_RESOURCE_BYTES - resource_bytes then
            resources[#resources + 1] = resource(item.resource_id, image)
            resource_bytes = resource_bytes + #image.pixels
            body_items[#body_items + 1] = item
            accepting_resources = #resources < MAX_RESOURCE_COUNT and
              resource_bytes < MAX_RESOURCE_BYTES
          else
            accepting_resources = false
          end
        end
      end
    else
      if item.type == "link" then item.action_id = prefix .. item.action_id end
      body_items[#body_items + 1] = item
    end
  end
  if #body_items > 0 then
    children[#children + 1] = { type = "rich_text", role = "notification-body", content = array(body_items) }
  end
  local buttons = {}
  local action_index = 0
  local has_default = false
  for index = 1, #notification.actions, 2 do
    local key, label = notification.actions[index], notification.actions[index + 1]
    if key == "default" then
      has_default = true
      routing[prefix .. "default"] = { "dbus", key }
    else
      local id = prefix .. "action-" .. tostring(action_index)
      action_index = action_index + 1
      routing[id] = { "dbus", key }
      buttons[#buttons + 1] = { type = "button", action_id = id, accessible_label = label,
        content = text(label, "button") }
    end
  end
  if #buttons > 0 then children[#children + 1] = { type = "row", gap = "small", children = array(buttons) } end
  local expanded = { type = "column", alignment = "start", gap = "small", children = array(children) }
  if has_default then expanded = { type = "action_region", action_id = prefix .. "default",
    accessible_label = "Open notification", content = expanded } end
  local publication = { context_id = "notification-" .. tostring(notification.id), priority = notification.priority,
    views = { expanded = expanded } }
  if history_visible_count == 0 and options.reveal_duration_ms > 0 then
    publication.presentation = { reveal = "expanded", duration_ms = options.reveal_duration_ms }
  end
  if #resources > 0 then publication.resources = array(resources) end
  return publication, routing
end

local function state_path()
  local root = os.getenv("XDG_STATE_HOME")
  if not root or root == "" then root = assert(os.getenv("HOME"), "HOME is not set") .. "/.local/state" end
  return root .. "/gisland/notifications-history.json"
end

local function secure_state_directory()
  local path = state_path()
  local directory = path:match("^(.*)/[^/]+$")
  assert(GLib.mkdir_with_parents(directory, tonumber("700", 8)) == 0, "cannot create history directory")
  local info = Gio.File.new_for_path(directory):query_info(
    "standard::type", Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, nil)
  assert(tostring(info:get_file_type()) == "DIRECTORY", "history directory is not a directory")
  assert(GLib.chmod(directory, tonumber("700", 8)) == 0, "cannot secure history directory")
  return path
end

local function json_type(node)
  return tostring(node:get_node_type())
end

local function history_record_from_node(node)
  assert(json_type(node) == "OBJECT", "history record must be an object")
  local object = node:get_object()
  assert(object:get_size() == 6, "history record has invalid fields")
  local function member(name)
    return assert(object:get_member(name), "history record member is missing: " .. name)
  end
  local sequence_node = member("sequence")
  local id_node = member("notification_id")
  local app_node = member("app_name")
  local summary_node = member("summary")
  local body_node = member("body")
  local received_node = member("received_at")
  assert(tostring(sequence_node:get_value_type()) == "gint64", "sequence must be an integer")
  assert(tostring(id_node:get_value_type()) == "gint64", "notification_id must be an integer")
  assert(tostring(app_node:get_value_type()) == "gchararray", "app_name must be a string")
  assert(tostring(summary_node:get_value_type()) == "gchararray", "summary must be a string")
  assert(tostring(body_node:get_value_type()) == "gchararray", "body must be a string")
  assert(tostring(received_node:get_value_type()) == "gdouble", "received_at must be a number")
  local sequence = math.tointeger(sequence_node:get_int())
  local notification_id = math.tointeger(id_node:get_int())
  local app_name = app_node:get_string()
  local summary = summary_node:get_string()
  local body = body_node:get_string()
  local received_at = received_node:get_double()
  assert(sequence and sequence > 0, "sequence must be positive")
  assert(notification_id and notification_id > 0 and notification_id <= UINT32_MAX,
    "notification_id must be a positive uint32")
  assert(#app_name <= MAX_HISTORY_TEXT_BYTES and #summary <= MAX_HISTORY_TEXT_BYTES and
    #body <= MAX_HISTORY_TEXT_BYTES, "history record string exceeds the byte limit")
  assert(received_at == received_at and math.abs(received_at) ~= math.huge, "received_at must be finite")
  return { sequence = sequence, notification_id = notification_id, app_name = app_name,
    summary = summary, body = body, received_at = received_at }
end

local function load_history()
  history = {}
  next_sequence = 1
  local path = secure_state_directory()
  if GLib.file_test(path, "IS_SYMLINK") then
    io.stderr:write("gisland-notifications: could not load notification history: history state is a symlink\n")
    return
  end
  if not GLib.file_test(path, "EXISTS") then return end
  local ok, message = pcall(function()
    local info = Gio.File.new_for_path(path):query_info(
      "standard::type,standard::size", Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, nil)
    assert(tostring(info:get_file_type()) == "REGULAR", "history state is not a regular file")
    local size = math.tointeger(info:get_size())
    assert(size and size >= 0 and size <= MAX_HISTORY_FILE_BYTES, "history state exceeds the byte limit")
    assert(GLib.chmod(path, tonumber("600", 8)) == 0, "cannot secure history state")
    local contents = assert(GLib.file_get_contents(path))
    assert(#contents <= MAX_HISTORY_FILE_BYTES, "history state changed while reading")
    local parser = Json.Parser.new()
    assert(parser:load_from_data(contents, -1), "invalid JSON")
    local root = parser:get_root()
    assert(json_type(root) == "OBJECT", "history state must be an object")
    local object = root:get_object()
    assert(object:get_size() == 3, "history state must contain exactly three members")
    local version = assert(object:get_member("version"), "history version is missing")
    local sequence = assert(object:get_member("next_sequence"), "next_sequence is missing")
    local records = assert(object:get_member("records"), "records are missing")
    assert(tostring(version:get_value_type()) == "gint64" and version:get_int() == 1,
      "unsupported history state version")
    assert(tostring(sequence:get_value_type()) == "gint64", "next_sequence must be an integer")
    local loaded_next = math.tointeger(sequence:get_int())
    assert(loaded_next and loaded_next > 0, "next_sequence must be positive")
    assert(json_type(records) == "ARRAY", "records must be an array")
    local values = records:get_array()
    assert(values:get_length() <= MAX_HISTORY_LIMIT, "history records exceed the limit")
    local loaded = {}
    local seen = {}
    local maximum = 0
    for index = 0, values:get_length() - 1 do
      local record = history_record_from_node(values:get_element(index))
      assert(not seen[record.sequence], "history record sequences must be unique")
      seen[record.sequence] = true
      maximum = math.max(maximum, record.sequence)
      if #loaded < options.history_limit then loaded[#loaded + 1] = record end
    end
    assert(loaded_next > maximum, "next_sequence must follow stored records")
    history = loaded
    next_sequence = loaded_next
  end)
  if not ok then
    history = {}
    next_sequence = 1
    io.stderr:write("gisland-notifications: could not load notification history: " .. tostring(message) .. "\n")
  end
end

local function history_snapshot()
  local builder = Json.Builder.new()
  builder:begin_object()
  builder:set_member_name("version") builder:add_int_value(1)
  builder:set_member_name("next_sequence") builder:add_int_value(next_sequence)
  builder:set_member_name("records") builder:begin_array()
  for _, record in ipairs(history) do
    builder:begin_object()
    builder:set_member_name("sequence") builder:add_int_value(record.sequence)
    builder:set_member_name("notification_id") builder:add_int_value(record.notification_id)
    builder:set_member_name("app_name") builder:add_string_value(record.app_name)
    builder:set_member_name("summary") builder:add_string_value(record.summary)
    builder:set_member_name("body") builder:add_string_value(record.body)
    builder:set_member_name("received_at") builder:add_double_value(record.received_at)
    builder:end_object()
  end
  builder:end_array() builder:end_object()
  local generator = Json.Generator.new()
  generator:set_root(builder:get_root())
  return generator:to_data() .. "\n"
end

local function start_history_save()
  if history_save_active or not history_save_pending then return end
  local snapshot = history_save_pending
  history_save_pending = nil
  history_writer_generation = history_writer_generation + 1
  local writer = { generation = history_writer_generation, snapshot = snapshot }
  history_save_active = writer
  local function begin_write()
    history_save_delay_source = nil
    if stopped or history_save_active ~= writer or writer.generation ~= history_writer_generation then
      return false
    end
    local file = Gio.File.new_for_path(state_path())
    local flags = Gio.FileCreateFlags.PRIVATE + Gio.FileCreateFlags.REPLACE_DESTINATION
    local cancellable = Gio.Cancellable.new()
    history_save_cancellable = cancellable
    local ok, message = pcall(function()
      file:replace_contents_async(snapshot.data, nil, false, flags, cancellable, function(candidate, result)
        if stopped or history_save_active ~= writer or writer.generation ~= history_writer_generation then
          return
        end
        local saved, save_message = pcall(function()
          candidate:replace_contents_finish(result)
          assert(GLib.chmod(state_path(), tonumber("600", 8)) == 0, "cannot secure history state")
        end)
        if not saved then
          gisland.log("error", "gisland-notifications: could not save notification history: " ..
            tostring(save_message))
        end
        history_save_active = nil
        history_save_cancellable = nil
        start_history_save()
      end)
    end)
    if not ok and not stopped and history_save_active == writer and
        writer.generation == history_writer_generation then
      history_save_active = nil
      history_save_cancellable = nil
      gisland.log("error", "gisland-notifications: could not save notification history: " .. tostring(message))
      start_history_save()
    end
    return false
  end
  local delay = math.tointeger(os.getenv("GISLAND_NOTIFICATIONS_TEST_WRITE_DELAY_MS") or "0") or 0
  if delay > 0 then
    history_save_delay_source = GLib.timeout_add(GLib.PRIORITY_DEFAULT, delay, begin_write)
  else
    begin_write()
  end
end

local function request_history_save()
  history_save_generation = history_save_generation + 1
  local ok, snapshot = pcall(history_snapshot)
  if not ok then
    gisland.log("error", "gisland-notifications: could not snapshot notification history: " .. tostring(snapshot))
    return
  end
  history_save_pending = { generation = history_save_generation, data = snapshot }
  history_save_latest = history_save_pending
  start_history_save()
end

local function shutdown_history_save()
  history_writer_generation = history_writer_generation + 1
  if history_save_delay_source then
    GLib.source_remove(history_save_delay_source)
    history_save_delay_source = nil
  end
  local cancellable = history_save_cancellable
  history_save_cancellable = nil
  history_save_active = nil
  history_save_pending = nil
  if cancellable then
    cancellable:cancel()
    local context = GLib.MainContext.default()
    if context:pending() then context:iteration(false) end
  end
  if not history_save_latest then return end
  local ok, message = pcall(function()
    local path = secure_state_directory()
    local flags = GLib.FileSetContentsFlags.CONSISTENT + GLib.FileSetContentsFlags.DURABLE
    local saved, save_message = GLib.file_set_contents_full(
      path, history_save_latest.data, flags, tonumber("600", 8))
    assert(saved, tostring(save_message))
    assert(GLib.chmod(path, tonumber("600", 8)) == 0, "cannot secure history state")
  end)
  if not ok then
    gisland.log("error", "gisland-notifications: could not save notification history during shutdown: " ..
      tostring(message))
  end
end

local function plain_body(value)
  local parsed = parse_body(value)
  local parts = {}
  for _, item in ipairs(parsed.items) do
    if item.type == "text" or item.type == "link" then parts[#parts + 1] = item.value
    elseif item.type == "inline_image" then
      local image = parsed.images[item.resource_id]
      if image and image[2] ~= "" then parts[#parts + 1] = image[2] end
    end
  end
  return bounded_utf8(table.concat(parts), MAX_HISTORY_TEXT_BYTES)
end

local function add_history(notification)
  local sequence = history_sequences[notification.id]
  local found = false
  if sequence then
    for index, record in ipairs(history) do
      if record.sequence == sequence then table.remove(history, index) found = true break end
    end
  end
  if not found then
    sequence = next_sequence
    next_sequence = next_sequence + 1
  end
  local record = {
    sequence = sequence, notification_id = notification.id,
    app_name = bounded_utf8(notification.app_name, MAX_HISTORY_TEXT_BYTES),
    summary = bounded_utf8(notification.summary, MAX_HISTORY_TEXT_BYTES),
    body = plain_body(notification.body), received_at = GLib.get_real_time() / 1000000.0,
  }
  table.insert(history, 1, record)
  while #history > options.history_limit do
    local evicted = table.remove(history)
    if history_sequences[evicted.notification_id] == evicted.sequence then
      history_sequences[evicted.notification_id] = nil
    end
  end
  history_sequences[notification.id] = sequence
  request_history_save()
end

local function remove_history(notification_id)
  local sequence = history_sequences[notification_id]
  if not sequence then return end
  history_sequences[notification_id] = nil
  for index, record in ipairs(history) do
    if record.sequence == sequence then
      table.remove(history, index)
      request_history_save()
      return
    end
  end
end

local function age(received_at, now)
  local elapsed = math.max(0, math.floor(now - received_at))
  if elapsed < 60 then return "maintenant" end
  if elapsed < 3600 then return tostring(elapsed // 60) .. " min" end
  if elapsed < 86400 then return tostring(elapsed // 3600) .. " h" end
  return tostring(elapsed // 86400) .. " j"
end

local function history_entry(record, now)
  local content = record.summary
  if record.body ~= "" then content = content ~= "" and (content .. " - " .. record.body) or record.body end
  local children = {
    { type = "row", gap = "small", children = array {
      text(record.app_name ~= "" and record.app_name:upper() or "APPLICATION", "caption"),
      { type = "spacer", flexible = true }, text(age(record.received_at, now), "caption"),
    } },
  }
  if content ~= "" then children[#children + 1] = text(bounded_utf8(content, MAX_HISTORY_TEXT_BYTES), "body") end
  return { type = "action_region",
    action_id = "history:" .. tostring(history_session_id) .. ":hide:" .. tostring(record.sequence),
    accessible_label = bounded_utf8("Masquer " .. (record.summary ~= "" and record.summary or
      (record.app_name ~= "" and record.app_name or "notification")), MAX_ACTION_LABEL_BYTES),
    content = { type = "column", alignment = "start", gap = "xsmall", children = array(children) } }
end

local function history_publication()
  local children = {
    { type = "row", gap = "small", children = array {
      text("Notifications", "title"), { type = "spacer", flexible = true },
      { type = "action_region", action_id = "history:" .. tostring(history_session_id) .. ":close-all",
        accessible_label = "Masquer toutes les notifications",
        content = { type = "icon", name = "close", accessible_label = "Masquer toutes les notifications" } },
    } },
  }
  local count = 0
  local now = GLib.get_real_time() / 1000000.0
  for _, record in ipairs(history) do
    if not history_hidden[record.sequence] and count < history_visible_count then
      children[#children + 1] = history_entry(record, now)
      count = count + 1
    end
  end
  if count == 0 then children[#children + 1] = text("Aucune notification", "caption") end
  return { context_id = "history", priority = 100, views = { expanded = {
    type = "column", alignment = "start", gap = "xsmall", children = array(children),
  } } }
end

local function cancel_source(source)
  if source then GLib.source_remove(source) end
end

local function reset_history()
  gisland.dismiss("history")
  history_visible_count = 0
  history_was_expanded = false
  history_hidden = {}
end

local function close_overlay()
  local host = arg and arg[0] or "gisland-lua-host"
  local directory = host:match("^(.*)/[^/]+$")
  local control = directory and (directory .. "/gislandctl") or "gislandctl"
  local flags = Gio.SubprocessFlags.STDOUT_SILENCE + Gio.SubprocessFlags.STDERR_SILENCE
  local ok, process = pcall(Gio.Subprocess.new, { control, "close" }, flags)
  if ok then
    process:wait_async(nil, function(candidate, result)
      pcall(function() candidate:wait_finish(result) end)
    end)
  end
end

local function close_history()
  cancel_source(history_open_timer) history_open_timer = nil
  cancel_source(history_inactivity_timer) history_inactivity_timer = nil
  reset_history()
  close_overlay()
end

local function rearm_inactivity()
  cancel_source(history_inactivity_timer)
  history_inactivity_timer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, HISTORY_INACTIVITY_TIMEOUT_MS,
    function() history_inactivity_timer = nil close_history() return false end)
end

local function show_more()
  if history_visible_count == 0 then
    history_session_id = history_session_id + 1
    history_hidden = {}
  end
  history_visible_count = math.min(history_visible_count + 1, options.history_visible_limit)
  gisland.publish(history_publication())
  if latest_visibility == "expanded-active" then history_was_expanded = true end
  if history_was_expanded then
    rearm_inactivity()
    return true
  end
  cancel_source(history_inactivity_timer) history_inactivity_timer = nil
  cancel_source(history_open_timer)
  history_open_timer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, HISTORY_OPEN_TIMEOUT_MS, function()
    history_open_timer = nil
    if history_visible_count > 0 and not history_was_expanded then close_history() end
    return false
  end)
  return true
end

local function allocate_id()
  if live_count >= UINT32_MAX then error("notification ID space exhausted") end
  local candidate = next_id
  while live[candidate] do candidate = candidate == UINT32_MAX and 1 or candidate + 1 end
  next_id = candidate == UINT32_MAX and 1 or candidate + 1
  return candidate
end

local function cancel_notification_timer(id)
  if timers[id] then GLib.source_remove(timers[id]) timers[id] = nil end
end

local function emit_signal(name, id, value)
  if not connection then return end
  local parameters = name == "ActionInvoked" and GLib.Variant("(us)", { id, tostring(value) }) or
    GLib.Variant("(uu)", { id, value })
  connection:emit_signal(nil, OBJECT_PATH, INTERFACE, name, parameters)
end

local function close_notification(id, reason)
  local notification = live[id]
  if not notification then return false end
  live[id] = nil
  live_count = live_count - 1
  cancel_notification_timer(id)
  routes[id] = nil
  history_sequences[id] = nil
  gisland.dismiss("notification-" .. tostring(id))
  emit_signal("NotificationClosed", id, reason)
  return true
end

local function notify(values)
  local app_name = require_string(values[1], MAX_APP_NAME_BYTES, "app_name")
  local replaces_id = math.tointeger(values[2])
  local app_icon = require_string(values[3], MAX_ICON_BYTES, "app_icon")
  local summary = require_string(values[4], MAX_SUMMARY_BYTES, "summary")
  local body = require_string(values[5], MAX_BODY_INPUT_BYTES, "body")
  if os.getenv("GISLAND_NOTIFICATIONS_TEST_INTERNAL_FAILURE") == "1" and
      summary == "trigger-internal-failure" then
    error("injected internal notification failure")
  end
  local actions = variant_array(values[6])
  local hints = values[7]
  local expire_timeout = math.tointeger(values[8])
  if not replaces_id or not expire_timeout then invalid("notification integer argument is invalid") end
  if #actions % 2 ~= 0 then invalid("actions must contain key and label pairs") end
  if #actions > MAX_ACTIONS * 2 then invalid("actions exceed the count limit") end
  for index, value in ipairs(actions) do
    require_string(value, index % 2 == 1 and MAX_ACTION_KEY_BYTES or MAX_ACTION_LABEL_BYTES,
      index % 2 == 1 and "action key" or "action label")
  end
  local id = replaces_id ~= 0 and live[replaces_id] and replaces_id or allocate_id()
  local urgency = math.tointeger(hints.urgency or 1)
  if urgency ~= 0 and urgency ~= 2 then urgency = 1 end
  local timeout
  if expire_timeout > 0 then timeout = expire_timeout
  elseif expire_timeout == 0 or urgency == 2 then timeout = nil
  else timeout = urgency == 0 and 5000 or 8000 end
  local notification = {
    id = id, app_name = app_name, summary = summary, body = body, actions = actions,
    priority = ({ [0] = 10, [1] = 20, [2] = 30 })[urgency], timeout = timeout,
    resident = hints.resident == true, transient = hints.transient == true,
  }
  local app_image = resolve_app_image(hints, app_icon)
  local publication, routing = notification_publication(notification, app_image)
  gisland.publish(publication)
  local replacing = live[id] ~= nil
  if replacing then cancel_notification_timer(id) end
  live[id] = notification
  if not replacing then live_count = live_count + 1 end
  routes[id] = routing
  if notification.transient then remove_history(id) else add_history(notification) end
  if history_visible_count > 0 then gisland.publish(history_publication()) end
  if timeout then
    timers[id] = GLib.timeout_add(GLib.PRIORITY_DEFAULT, timeout, function()
      timers[id] = nil
      close_notification(id, 1)
      return false
    end)
  end
  return id
end

local function invoke_route(action_id)
  if action_id:match("^history:") then
    if history_visible_count == 0 then return false end
    local session, operation, sequence = action_id:match("^history:(%d+):([%a%-]+):?(%d*)$")
    if tonumber(session) ~= history_session_id then return false end
    if operation == "close-all" and sequence == "" then close_history() return true end
    if operation ~= "hide" or sequence == "" then return false end
    sequence = tonumber(sequence)
    local visible = 0
    local found = false
    for _, record in ipairs(history) do
      if not history_hidden[record.sequence] and visible < history_visible_count then
        visible = visible + 1
        if record.sequence == sequence then found = true break end
      end
    end
    if not found then return false end
    history_hidden[sequence] = true
    history_visible_count = history_visible_count - 1
    if history_visible_count == 0 then close_history()
    else gisland.publish(history_publication()) rearm_inactivity() end
    return true
  end
  local id = tonumber(action_id:match("^notification%-(%d+):"))
  local notification = id and live[id]
  local route = id and routes[id] and routes[id][action_id]
  if not notification or not route then return false end
  if route[1] == "close" then return close_notification(id, 2) end
  if route[1] == "uri" then
    if not route[2]:match("^https?://") and not route[2]:match("^mailto:") then return false end
    local ok, result = pcall(Gio.AppInfo.launch_default_for_uri, route[2], nil)
    return ok and not not result
  end
  if route[1] ~= "dbus" then return false end
  emit_signal("ActionInvoked", id, route[2])
  if not notification.resident then close_notification(id, 2) end
  return true
end

local function return_error(invocation, name, message)
  invocation:return_dbus_error(name, tostring(message))
end

local function raw_image_variant(value)
  if value:get_type_string() ~= "(iiibiiay)" or value:n_children() ~= 7 then
    error("image data has the wrong variant signature")
  end
  local raw = {}
  for index = 0, 5 do raw[index + 1] = value:get_child_value(index).value end
  local bytes = value:get_child_value(6)
  local width = math.tointeger(raw[1])
  local height = math.tointeger(raw[2])
  local rowstride = math.tointeger(raw[3])
  local has_alpha = raw[4]
  local bits = math.tointeger(raw[5])
  local channels = math.tointeger(raw[6])
  if not width or not height or not rowstride or type(has_alpha) ~= "boolean" or not bits or not channels then
    error("image data has invalid field types")
  end
  if width <= 0 or height <= 0 or width > 16384 or height > 16384 or bits ~= 8 or
      (channels ~= 3 and channels ~= 4) or has_alpha ~= (channels == 4) then
    error("image metadata is invalid")
  end
  if width > math.maxinteger // channels or rowstride < width * channels or
      rowstride > MAX_IMAGE_INPUT_BYTES or height > MAX_IMAGE_INPUT_BYTES // rowstride then
    error("image data exceeds the byte limit")
  end
  local required = rowstride * height
  if bytes:get_type_string() ~= "ay" or bytes:n_children() ~= required or required > MAX_IMAGE_INPUT_BYTES then
    error("image byte count does not match its rowstride")
  end
  raw[7] = bytes:get_data_as_bytes():get_data()
  if #raw[7] ~= required then error("image byte materialization failed") end
  return raw
end

local function unpack_hints(value)
  local count = math.tointeger(value:n_children())
  if not count or count > MAX_HINTS then invalid("hints exceed the count limit") end
  local hints = {}
  for index = 0, count - 1 do
    local entry = value:get_child_value(index)
    local key = entry:get_child_value(0).value
    if #key > MAX_HINT_KEY_BYTES then invalid("hint key exceeds the byte limit") end
    local item = entry:get_child_value(1):get_variant()
    if key == "image-data" or key == "image_data" or key == "icon_data" then
      local ok, raw = pcall(raw_image_variant, item)
      if ok then hints[key] = raw end
    elseif (key == "image-path" or key == "image_path" or key == "desktop-entry") and
        item:get_type_string() == "s" then
      local text_value = item.value
      if #text_value <= MAX_ICON_BYTES then hints[key] = text_value end
    elseif key == "urgency" and item:get_type_string() == "y" then
      hints[key] = item.value
    elseif (key == "resident" or key == "transient") and item:get_type_string() == "b" then
      hints[key] = item.value
    end
  end
  return hints
end

local function unpack_actions(value)
  if value:get_type_string() ~= "as" then invalid("actions have the wrong variant signature") end
  local count = math.tointeger(value:n_children())
  if not count or count > MAX_ACTIONS * 2 then invalid("actions exceed the count limit") end
  local actions = {}
  for index = 0, count - 1 do
    local child = value:get_child_value(index)
    if child:get_type_string() ~= "s" then invalid("actions must contain strings") end
    local item = child.value
    require_string(item, index % 2 == 0 and MAX_ACTION_KEY_BYTES or MAX_ACTION_LABEL_BYTES,
      index % 2 == 0 and "action key" or "action label")
    actions[#actions + 1] = item
  end
  return actions
end

local function method_call(_, _, _, _, method, parameters, invocation)
  if stopped then return_error(invocation, "org.freedesktop.DBus.Error.Failed", "service unavailable") return end
  local ok, message = pcall(function()
    if method == "GetCapabilities" then
      invocation:return_value(GLib.Variant("(as)", { capabilities }))
    elseif method == "GetServerInformation" then
      invocation:return_value(GLib.Variant("(ssss)", { "gisland", "gisland", "1.0.0", "1.2" }))
    elseif method == "Notify" then
      local values = {}
      for index = 0, 4 do values[index + 1] = parameters:get_child_value(index).value end
      values[6] = unpack_actions(parameters:get_child_value(5))
      values[7] = unpack_hints(parameters:get_child_value(6))
      values[8] = parameters:get_child_value(7).value
      invocation:return_value(GLib.Variant("(u)", { notify(values) }))
    elseif method == "CloseNotification" then
      if not close_notification(math.tointeger(parameters.value[1]), 3) then
        return_error(invocation, "org.freedesktop.Notifications.Error.NonExistent", "")
        return
      end
      invocation:return_value(nil)
    else
      return_error(invocation, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method: " .. method)
    end
  end)
  if not ok then
    if type(message) == "table" and message.dbus_error then
      return_error(invocation, message.dbus_error, message.message or "")
    else
      return_error(invocation, "org.freedesktop.DBus.Error.Failed", message)
    end
  end
end

local function configure(config)
  for name in pairs(config) do
    if options[name] == nil then error("unknown notification option: " .. name) end
  end
  for name, default in pairs(options) do
    local value = config[name]
    if value == nil then value = default end
    if math.type(value) ~= "integer" then error(name .. " must be an integer") end
    options[name] = value
  end
  if options.reveal_duration_ms < 0 or options.reveal_duration_ms > 60000 then
    error("reveal_duration_ms must be an integer between 0 and 60000")
  end
  if options.history_limit < 1 or options.history_limit > MAX_HISTORY_LIMIT then
    error("history_limit must be an integer between 1 and 1000")
  end
  if options.history_visible_limit < 1 or options.history_visible_limit > 5 then
    error("history_visible_limit must be an integer between 1 and 5")
  end
  if options.history_visible_limit > options.history_limit then
    error("history_visible_limit must not exceed history_limit")
  end
end

local function request_name()
  connection = Gio.bus_get_sync(Gio.BusType.SESSION, nil)
  connection.on_closed = function()
    if not stopped then
      io.stderr:write("gisland-notifications: lost org.freedesktop.Notifications session bus connection\n")
      os.exit(1, true)
    end
  end
  local node = Gio.DBusNodeInfo.new_for_xml(introspection_xml)
  registration_id = connection:register_object(
    OBJECT_PATH, node.interfaces[1], lgi.GObject.Closure(method_call), nil, nil)
  assert(registration_id and registration_id ~= 0, "could not register notification D-Bus object")
  local result = connection:call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
    "org.freedesktop.DBus", "RequestName", GLib.Variant("(su)", { BUS_NAME, 4 }),
    GLib.VariantType.new("(u)"), Gio.DBusCallFlags.NONE, 2000, nil)
  local reply = math.tointeger(result.value[1])
  if reply ~= 1 and reply ~= 4 then
    connection:unregister_object(registration_id)
    registration_id = nil
    error("could not own org.freedesktop.Notifications")
  end
  name_subscription = Gio.bus_watch_name_on_connection(connection, BUS_NAME,
    Gio.BusNameWatcherFlags.NONE, nil, lgi.GObject.Closure(function()
      if stopped then return end
      io.stderr:write("gisland-notifications: could not own org.freedesktop.Notifications\n")
      os.exit(1, true)
    end))
  local release_delay = math.tointeger(os.getenv("GISLAND_NOTIFICATIONS_TEST_RELEASE_NAME_AFTER_MS") or "0") or 0
  if release_delay > 0 then
    GLib.timeout_add(GLib.PRIORITY_DEFAULT, release_delay, function()
      connection:call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "ReleaseName", GLib.Variant("(s)", { BUS_NAME }), GLib.VariantType.new("(u)"),
        Gio.DBusCallFlags.NONE, 1000, nil)
      return false
    end)
  end
end

local function shutdown()
  stopped = true
  cancel_source(history_open_timer) history_open_timer = nil
  cancel_source(history_inactivity_timer) history_inactivity_timer = nil
  for id in pairs(timers) do cancel_notification_timer(id) end
  shutdown_history_save()
  if connection then
    connection.on_closed = nil
    if name_subscription then Gio.bus_unwatch_name(name_subscription) name_subscription = nil end
    if registration_id then connection:unregister_object(registration_id) registration_id = nil end
    pcall(function() connection:close(nil) end)
    connection = nil
  end
end

return gisland.module {
  init = function(config)
    configure(config)
    load_history()
    request_name()
  end,
  actions = { ["show-more"] = show_more },
  fallback_action = function(action_id) return invoke_route(action_id) end,
  visibility = function(value)
    latest_visibility = value
    if value == "expanded-active" and history_visible_count > 0 then
      cancel_source(history_open_timer) history_open_timer = nil
      history_was_expanded = true
      rearm_inactivity()
    elseif value == "hidden" and history_was_expanded then
      cancel_source(history_open_timer) history_open_timer = nil
      cancel_source(history_inactivity_timer) history_inactivity_timer = nil
      reset_history()
    end
  end,
  shutdown = shutdown,
}
