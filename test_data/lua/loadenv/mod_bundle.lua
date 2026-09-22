-- envy.loadenv involves no bundle: ENVY_BUNDLE stays nil.
local M = {}
M.FROM_BUNDLE = ENVY_BUNDLE ~= nil
return M
