-- A fragment's globals live in its import sandbox, not _G: a module it loads out of a
-- bundle must still read them.
VENDOR_ROOT = "third_party"

BUNDLES = { tools = { identity = "local.tools@r1",
                      source = "../../../bundles/local-bundle" } }

local m = envy.loadenv_bundle("tools", "lib.reads_global")

PACKAGES = { m.entry() }
