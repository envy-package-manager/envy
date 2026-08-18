#pragma once

#include "cmds/cmd_cache.h"
#include "cmds/cmd_deploy.h"
#include "cmds/cmd_export.h"
#include "cmds/cmd_extract.h"
#include "cmds/cmd_fetch.h"
#include "cmds/cmd_git_resolve.h"
#include "cmds/cmd_hash.h"
#include "cmds/cmd_import.h"
#include "cmds/cmd_init.h"
#include "cmds/cmd_install.h"
#include "cmds/cmd_lua.h"
#include "cmds/cmd_merge_depot.h"
#include "cmds/cmd_mirror_envy.h"
#include "cmds/cmd_package.h"
#include "cmds/cmd_product.h"
#include "cmds/cmd_run.h"
#include "cmds/cmd_shell.h"
#include "cmds/cmd_sync.h"
#include "cmds/cmd_use.h"
#include "cmds/cmd_version.h"
#ifdef ENVY_FUNCTIONAL_TESTER
#include "cmds/cmd_cache_functional_test.h"
#include "cmds/cmd_trace_schema.h"
#endif
#include "tui.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace envy {

struct cli_args {
  using cmd_cfg_t = std::variant<cmd_package::cfg,
                                 cmd_cache::cfg,
                                 cmd_deploy::cfg,
                                 cmd_export::cfg,
                                 cmd_extract::cfg,
                                 cmd_fetch::cfg,
                                 cmd_git_resolve::cfg,
                                 cmd_hash::cfg,
                                 cmd_import::cfg,
                                 cmd_init::cfg,
                                 cmd_install::cfg,
                                 cmd_lua::cfg,
                                 cmd_merge_depot::cfg,
                                 cmd_mirror_envy::cfg,
                                 cmd_product::cfg,
                                 cmd_run::cfg,
                                 cmd_shell::cfg,
                                 cmd_sync::cfg,
                                 cmd_use::cfg,
                                 cmd_version::cfg
#ifdef ENVY_FUNCTIONAL_TESTER
                                 ,
                                 cmd_cache_ensure_package::cfg,
                                 cmd_cache_ensure_spec::cfg,
                                 cmd_trace_schema::cfg
#endif
                                 >;

  std::optional<cmd_cfg_t> cmd_cfg;
  std::optional<std::filesystem::path> cache_root;  // Global cache root override
  std::optional<tui::level> verbosity;
  bool decorated_logging{ false };
  std::vector<tui::trace_output_spec> trace_outputs;
  std::string cli_output;
};

cli_args cli_parse(int argc, char **argv);

}  // namespace envy
