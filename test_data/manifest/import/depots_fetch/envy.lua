-- @envy bin "tools"
-- A depot FETCH closure keeps reading this file's globals once a superproject adopts it.
DEPOT_NAME = "depots_fetch"
PACKAGE_DEPOTS = { { FETCH = function(ctx) return DEPOT_NAME end } }
