IDENTITY = "local.failing_setup_a@r0"
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, opts) return false end,
    INSTALL = function(pkg_dir, opts) error("setup A refused") end,
  },
}
