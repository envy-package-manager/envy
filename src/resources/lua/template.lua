-- envy.template(str, values): fills each {{name}} in str from values.
return function(str, values)
  if type(str) ~= "string" then
    error("envy.template: first argument must be a string", 2)
  end
  if type(values) ~= "table" then
    error("envy.template: second argument must be a table", 2)
  end

  local function normalize_key(raw)
    local trimmed = raw:match("^%s*(.-)%s*$")
    if not trimmed or trimmed == "" then
      error("envy.template: placeholder cannot be empty", 2)
    end
    if not trimmed:match("^[%a_][%w_]*$") then
      error("envy.template: placeholder '" .. trimmed .. "' contains invalid characters", 2)
    end
    return trimmed
  end

  local function ensure_pairs(str)
    local open_count = 0
    local i = 1
    while i <= #str do
      if str:sub(i, i+1) == "{{" then
        open_count = open_count + 1
        i = i + 2
      elseif str:sub(i, i+1) == "}}" then
        open_count = open_count - 1
        if open_count < 0 then
          error("envy.template: unmatched '}}' at position " .. i, 2)
        end
        i = i + 2
      else
        i = i + 1
      end
    end
    if open_count > 0 then
      error("envy.template: unmatched '{{' (missing closing '}}')", 2)
    end
  end

  ensure_pairs(str)

  local function replacer(token)
    local key = normalize_key(token)
    local value = values[key]
    if value == nil then
      error("envy.template: missing value for placeholder '" .. key .. "'", 2)
    end
    return tostring(value)
  end

  return (str:gsub("{{(.-)}}", replacer))
end
