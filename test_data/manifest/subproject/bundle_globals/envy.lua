-- A subproject's globals live in its sandbox, not _G: a module it loads out of a bundle
-- must still read them, and its envy.
VENDOR_ROOT = "third_party"

BUNDLES = { tools = { identity = "local.tools@r1",
                      source = "../../../bundles/local-bundle" } }

local m = envy.loadenv_bundle("tools", "lib.reads_global")
local e = envy.loadenv_bundle("tools", "lib.reads_envy")

PACKAGES = { m.entry(), e.entry() }
