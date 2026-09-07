IDENTITY = "local.failing_setup_b@r0"
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, opts) return false end,
    INSTALL = function(pkg_dir, opts) error("setup B refused") end,
  },
}
