-- @envy bin "tools"
-- Loaded by test_data/manifest/subproject fixtures; also loadable standalone, which is
-- what the envy.IS_SUBPROJECT gate below distinguishes.

BUNDLES = {
  tools = { identity = "sub.bundle@r1", source = "bundles/tools" },
}

PACKAGES = {
  { spec = "sub.tool@r1", source = "specs/tool.lua" },
  { spec = "sub.bundled@r1", bundle = "tools" },
  { spec = "sub.opt@r1", source = "specs/opt.lua", options = { version = "1.0" } },
}

if not envy.IS_SUBPROJECT then
  PACKAGES[#PACKAGES + 1] = { spec = "sub.standalone@r1", source = "specs/standalone.lua" }
end

ROOT_SEEN = envy.PROJECT_ROOT -- read while this file runs

function probe() -- called from the superproject, after this file has run
  return { spec = "sub.probe@r1", source = envy.PROJECT_ROOT .. "/specs/probe.lua",
           options = { sub = envy.IS_SUBPROJECT } }
end
