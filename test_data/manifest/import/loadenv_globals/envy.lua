-- envy.loadenv from a fragment: the helper beside it reads what this file assigned.
VENDOR_ROOT = "third_party"

local m = envy.loadenv("helper")

PACKAGES = { m.entry() }
