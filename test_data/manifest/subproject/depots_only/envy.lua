-- @envy bin "tools"
-- A fragment that assigns neither PACKAGES nor BUNDLES: the sandbox's __index would
-- otherwise hand back the importing manifest's own globals.
PACKAGE_DEPOTS = { "https://depot.invalid/index.txt" }
