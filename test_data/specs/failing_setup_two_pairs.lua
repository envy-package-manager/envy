IDENTITY = "local.failing_setup_two@r0"
USER_MANAGED = true
SETUP = {
  one = {
    CHECK = function(pkg_dir, opts) return false end,
    INSTALL = function(pkg_dir, opts) error("setup one refused") end,
  },
  two = {
    CHECK = function(pkg_dir, opts) return false end,
    INSTALL = function(pkg_dir, opts) error("setup two refused") end,
  },
}
