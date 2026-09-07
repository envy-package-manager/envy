IDENTITY = "local.failing_setup_consumer@r0"
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, opts) return true end,
    INSTALL = function(pkg_dir, opts) end,
  },
}
DEPENDENCIES = {
  { spec = "local.failing_setup_a@r0", source = "failing_setup_a.lua", setup = { "main" } },
}
