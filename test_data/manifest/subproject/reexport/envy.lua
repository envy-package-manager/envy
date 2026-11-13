-- Re-exports a nested subproject wholesale: what pairs() yields must be safe to copy.
for k, v in pairs(envy.subproject("../sub")) do _ENV[k] = v end
ROOT_SEEN_HERE = envy.PROJECT_ROOT
