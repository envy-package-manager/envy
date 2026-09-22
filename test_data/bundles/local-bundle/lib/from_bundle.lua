-- Hands its own ENVY_BUNDLE back through an entry, for a test to read.
local M = {}

function M.entry()
  return { spec = "local.generic@r1", bundle = ENVY_BUNDLE.alias,
           options = { identity = ENVY_BUNDLE.identity, alias = ENVY_BUNDLE.alias,
                       root_tail = ENVY_BUNDLE.root:match("[^/\\]+$") } }
end

return M
