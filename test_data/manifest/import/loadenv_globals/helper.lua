local M = {}

function M.entry()
  return { spec = "a.one@v1", source = "/fake/r.lua",
           options = { seen = VENDOR_ROOT or "<nil>" } }
end

return M
