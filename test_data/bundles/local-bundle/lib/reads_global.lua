-- Reads a global at call time, the way lib.github's entry builders read VENDOR_ROOT.
local M = {}

function M.entry()
  return { spec = "local.generic@r1", bundle = "tools",
           options = { seen = VENDOR_ROOT or "<nil>" } }
end

return M
