-- @envy bin "tools"
-- Imported by test_data/manifest/import fixtures; also loadable standalone, which is
-- what the ENVY_IMPORTER gate below distinguishes.

BUNDLES = {
  tools = { identity = "sub.bundle@r1", source = "bundles/tools" },
}

PACKAGES = {
  { spec = "sub.tool@r1", source = "specs/tool.lua" },
  { spec = "sub.bundled@r1", bundle = "tools" },
  { spec = "sub.opt@r1", source = "specs/opt.lua", options = { version = "1.0" } },
}

if not ENVY_IMPORTER then
  PACKAGES[#PACKAGES + 1] = { spec = "sub.standalone@r1", source = "specs/standalone.lua" }
end
