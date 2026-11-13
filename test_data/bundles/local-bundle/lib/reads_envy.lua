-- Reads envy at call time: a subproject's own, when that subproject loaded the module.
local M = {}

function M.entry()
  return { spec = "local.envy_probe@r1", source = envy.PROJECT_ROOT .. "/probe.lua",
           options = { sub = envy.IS_SUBPROJECT } }
end

return M
