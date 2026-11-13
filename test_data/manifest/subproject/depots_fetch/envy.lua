-- @envy bin "tools"
-- A depot FETCH closure keeps reading this file's globals and envy once a superproject
-- adopts it.
DEPOT_NAME = "depots_fetch"
PACKAGE_DEPOTS = { { FETCH = function(ctx)
  return DEPOT_NAME .. "|" .. tostring(envy.IS_SUBPROJECT) .. "|" .. envy.PROJECT_ROOT
end } }
