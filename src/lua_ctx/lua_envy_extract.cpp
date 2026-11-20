#include "lua_envy_extract.h"

#include "lua_phase_context.h"
#include "pkg.h"
#include "sol_util.h"
#include "tui.h"
#include "tui_actions.h"
#include "util.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace envy {
namespace {

extract_options parse_optional_opts(sol::optional<sol::table> const &opts_table,
                                    std::string const &context) {
  return opts_table ? lua_envy_extract_parse_opts(*opts_table, context)
                    : extract_options{};
}

}  // namespace

extract_options lua_envy_extract_parse_opts(sol::table const &opts,
                                            std::string const &context) {
  int const strip{ sol_util_get_optional<int>(opts, "strip", context).value_or(0) };
  if (strip < 0) { throw_runtime_error({ context, ": strip must be non-negative" }); }

  auto const list{ [&](char const *key) {  // an empty list is a mistake, not "everything"
    auto entries{ sol_util_get_string_list(opts, key, context) };
    if (entries.empty() && opts[key].valid()) {
      throw_runtime_error({ context, ": '", key, "' must list at least one entry" });
    }
    return entries;
  } };

  return { .strip_components = strip,
           .selectors = list("only"),
           .archives = list("archives") };
}

void lua_envy_extract_install(sol::table &envy_table) {
  // envy.extract(archive_path, dest_dir, opts?) - Single archive extraction
  envy_table["extract"] = [](std::string const &archive_path_str,
                             std::string const &dest_dir_str,
                             sol::optional<sol::table> opts_table,
                             sol::this_state L) -> int {
    extract_options opts{ parse_optional_opts(opts_table, "envy.extract") };

    std::filesystem::path const archive_path{
      lua_phase_context_resolve(L, archive_path_str)
    };
    std::filesystem::path const dest_dir{ lua_phase_context_resolve(L, dest_dir_str) };

    if (!std::filesystem::exists(archive_path)) {
      throw_runtime_error({ "envy.extract: file not found: ", archive_path });
    }

    // A row only in a phase context; elsewhere there is no package to draw it under.
    phase_context const *ctx{ lua_phase_context_get(L) };
    pkg const *p{ ctx ? ctx->p : nullptr };
    return static_cast<int>(
        tui_actions::extract_tracked(archive_path,
                                     dest_dir,
                                     opts,
                                     p ? p->tui_section : tui::kInvalidSection,
                                     p ? p->cfg->identity : std::string{},
                                     archive_path.filename().string()));
  };

  // envy.extract_all(src_dir, dest_dir, opts?) - Extract all archives in directory
  envy_table["extract_all"] = [](std::string const &src_dir_str,
                                 std::string const &dest_dir_str,
                                 sol::optional<sol::table> opts_table,
                                 sol::this_state L) {
    extract_options const opts{ parse_optional_opts(opts_table, "envy.extract_all") };

    std::filesystem::path const src_dir{ lua_phase_context_resolve(L, src_dir_str) };
    std::filesystem::path const dest_dir{ lua_phase_context_resolve(L, dest_dir_str) };

    if (!std::filesystem::exists(src_dir)) {
      throw_runtime_error({ "envy.extract_all: source directory not found: ", src_dir });
    }

    // Get identity and section from phase context if available
    phase_context const *ctx{ lua_phase_context_get(L) };
    std::string identity{ ctx && ctx->p ? ctx->p->cfg->identity : "" };
    tui::section_handle section{ ctx && ctx->p ? ctx->p->tui_section
                                               : tui::kInvalidSection };

    extract_all_archives(src_dir, dest_dir, opts, identity, section);
  };
}

}  // namespace envy
