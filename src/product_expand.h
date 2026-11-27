#pragma once

#include "pkg_phase.h"
#include "shell.h"
#include "tui.h"
#include "util.h"

#include "sol/forward.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace envy {

struct pkg;

// A product's provider and the needed_by of the edge granting it.
struct product_grant {
  pkg *provider;
  pkg_phase needed_by{ pkg_phase::none };
};

// Who provides `name` to `consumer` in `phase`: itself, a product dependency, or a direct
// edge to the registry's provider. Throws "<what>: why", appending `hint` if none exists.
product_grant product_authorize(pkg *consumer,
                                pkg_phase phase,
                                std::string const &name,
                                std::string_view what,
                                bool trace = true,
                                std::string_view hint = {});

// Emits lua_ctx_product_access for one access to `name`.
void product_trace(pkg *consumer,
                   std::string const &name,
                   pkg_phase phase,
                   product_grant const &grant,
                   bool allowed,
                   std::string const &reason);

// `provider`'s payload dir; for `consumer` itself before install, the predicted one.
std::filesystem::path product_payload_dir(pkg *consumer, pkg *provider);

// Context for expanding one phase's shell text.
struct product_expand_cfg {
  pkg *p;  // null outside a package's phase
  pkg_phase phase;
  std::string_view what;  // error prefix, e.g. "<identity> BUILD"
  resolved_shell const &shell;
  shell_env_t const &env;                   // PATH for bare programs
  std::filesystem::path const &cwd;         // base for relative PATH entries
  std::filesystem::path const &state_root;  // command functions' pkg_state_dir root
  lua_State *held{ nullptr };               // p's Lua state if the caller holds its lock
};

// Expands `{{name}}`: a path product as raw text, a command product as quoted argv (whole
// words; refused under a custom shell); `{{{{x}}}}` as `{{x}}`.
std::string product_expand(std::string_view text, product_expand_cfg const &cfg);

// Checks `text` as product_expand would, on predicted paths without rendering; with no
// `shell`, skips shell-specific rules.
void product_expand_check(pkg *p,
                          pkg_phase phase,
                          std::string_view text,
                          std::string_view what,
                          std::optional<resolved_shell> const &shell);

// Expands and runs a phase's shell text in `p`'s shell and phase environment.
void product_run_phase_script(pkg *p,
                              pkg_phase phase,
                              std::string_view verb,
                              std::string_view label,
                              std::string_view script,
                              std::filesystem::path const &cwd,
                              std::filesystem::path const &state_root,
                              lua_State *held,
                              tui::section_handle section,
                              std::filesystem::path const &cache_root);

}  // namespace envy
