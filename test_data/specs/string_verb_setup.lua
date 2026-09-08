IDENTITY = "local.string_verb_setup@r0"
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = "exit 0",
    INSTALL = function(pkg_dir, opts) end,
  },
}
