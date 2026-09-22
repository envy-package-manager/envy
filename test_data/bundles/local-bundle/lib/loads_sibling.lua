-- A bundled module reaching a file beside it: envy.loadenv resolves no bundle.
OUTER_ASSIGNED = "outer"
local sib = envy.loadenv("sibling_probe")

local M = {}
function M.entry()
  return { spec = "local.generic@r1", bundle = "tools",
           options = { bundle = sib.bundle, outer = sib.outer } }
end
return M
