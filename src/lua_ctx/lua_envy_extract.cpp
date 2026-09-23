#include "lua_envy_extract.h"

#include "lua_phase_context.h"
#include "pkg.h"
#include "sol_util.h"
#include "tui.h"
#include "tui_actions.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace envy {
namespace {

// Resolve relative path to phase working directory (from registry, or stage_dir fallback)
std::filesystem::path resolve_relative(std::filesystem::path const &path,
                                       sol::this_state L) {
  if (path.is_absolute()) { return path; }
  phase_context const *ctx{ lua_phase_context_get(L) };
  if (ctx && ctx->run_dir) { return *ctx->run_dir / path; }
  if (ctx && ctx->p && ctx->p->lock) { return ctx->p->lock->stage_dir() / path; }
  return std::filesystem::current_path() / path;
}

extract_options parse_optional_opts(sol::optional<sol::table> const &opts_table,
                                    std::string const &context) {
  return opts_table ? lua_envy_extract_parse_opts(*opts_table, context)
                    : extract_options{};
}

}  // namespace

extract_options lua_envy_extract_parse_opts(sol::table const &opts,
                                            std::string const &context) {
  int const strip{ sol_util_get_optional<int>(opts, "strip", context).value_or(0) };
  if (strip < 0) { throw std::runtime_error(context + ": strip must be non-negative"); }

  auto const list{ [&](char const *key) {  // an empty list is a mistake, not "everything"
    auto entries{ sol_util_get_string_list(opts, key, context) };
    if (entries.empty() && opts[key].valid()) {
      throw std::runtime_error(context + ": '" + key + "' must list at least one entry");
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

    std::filesystem::path const archive_path{ resolve_relative(archive_path_str, L) };
    std::filesystem::path const dest_dir{ resolve_relative(dest_dir_str, L) };

    if (!std::filesystem::exists(archive_path)) {
      throw std::runtime_error("envy.extract: file not found: " + archive_path.string());
    }

    // Set up progress tracking if in phase context
    phase_context const *ctx{ lua_phase_context_get(L) };
    pkg *p{ ctx ? ctx->p : nullptr };
    std::optional<tui_actions::extract_progress_tracker> tracker;
    if (p && p->tui_section) {
      tracker.emplace(p->tui_section, p->cfg->identity, archive_path.filename().string());
    }

    if (tracker) { opts.progress = std::ref(*tracker); }

    auto const count{ extract(archive_path, dest_dir, opts) };
    if (tracker) { tracker->finish(); }
    return static_cast<int>(count);
  };

  // envy.extract_all(src_dir, dest_dir, opts?) - Extract all archives in directory
  envy_table["extract_all"] = [](std::string const &src_dir_str,
                                 std::string const &dest_dir_str,
                                 sol::optional<sol::table> opts_table,
                                 sol::this_state L) {
    extract_options const opts{ parse_optional_opts(opts_table, "envy.extract_all") };

    std::filesystem::path const src_dir{ resolve_relative(src_dir_str, L) };
    std::filesystem::path const dest_dir{ resolve_relative(dest_dir_str, L) };

    if (!std::filesystem::exists(src_dir)) {
      throw std::runtime_error("envy.extract_all: source directory not found: " +
                               src_dir.string());
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
