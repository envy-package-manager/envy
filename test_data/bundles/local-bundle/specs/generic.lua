IDENTITY = "local.generic@r1"

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return false end,
    INSTALL = function(pkg_dir, options) end,
  },
}
