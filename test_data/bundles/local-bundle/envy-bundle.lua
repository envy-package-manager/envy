-- A 'local.' bundle, read where it stands: no cache entry, so a unit test can load
-- a helper out of it without writing anything.
BUNDLE = "local.tools@r1"

SPECS = {
  ["local.generic@r1"] = "specs/generic.lua",
}
