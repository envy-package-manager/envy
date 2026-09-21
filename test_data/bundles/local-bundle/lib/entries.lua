-- An entry builder: one line per dependency in the consuming manifest.
local M = {}

function M.entry(name)
  return { spec = "local.generic@r1", bundle = "tools", options = { name = name } }
end

return M
