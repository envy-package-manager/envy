-- @envy bin "tools"
-- A subproject that assigns neither PACKAGES nor BUNDLES: the sandbox's __index would
-- otherwise hand back the superproject's own globals.
PACKAGE_DEPOTS = { "https://depot.invalid/index.txt" }
