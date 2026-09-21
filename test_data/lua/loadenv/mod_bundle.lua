-- envy.loadenv involves no bundle, so the module is told of none.
local M = {}
M.FROM_BUNDLE = ENVY_BUNDLE ~= nil
return M
