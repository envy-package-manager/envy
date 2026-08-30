#include "cmd_cache.h"

#include "cache.h"
#include "manifest.h"
#include "platform.h"
#include "tui.h"
#include "util.h"

#include "cli_parse.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace envy {

namespace {

struct row {
  std::string label;
  std::size_t index{ 0 };  // into the parallel-scan result vector
  platform::dir_size size;
  std::string size_text;
};

// Child directories of `dir`, name-sorted, symlinks dropped: a linked tree is
// billed to whoever owns it, and following one would double-count.
std::vector<platform::dir_entry> child_dirs(std::filesystem::path const &dir) {
  auto entries{ platform::dir_list(dir) };
  std::erase_if(entries,
                [](platform::dir_entry const &e) { return !e.is_dir || e.is_symlink; });
  std::ranges::sort(entries, {}, &platform::dir_entry::name);
  return entries;
}

void print_section(char const *title,
                   std::vector<row> const &rows,
                   int label_width,
                   int size_width) {
  tui::print_stdout("\n%s\n", title);
  if (rows.empty()) {
    tui::print_stdout("  (none)\n");
    return;
  }
  for (auto const &r : rows) {
    tui::print_stdout("  %-*s  %*s\n",
                      label_width,
                      r.label.c_str(),
                      size_width,
                      r.size_text.c_str());
  }
}

}  // namespace

cli_cmd &cmd_cache::register_cli(cli_cmd &app, cfg &c) {
  auto &sub{ app.sub("cache", "Show cache location and disk usage") };

  auto const root_flag{ sub.flag("--root",
                                 c.want_root,
                                 "Print the resolved cache root and nothing else") };
  auto const user_wide_flag{ sub.flag("--user-wide-root",
                                      c.want_user_wide,
                                      "Print the user-wide cache root and nothing else") };
  auto const local_flag{ sub.flag("--local",
                                  c.want_local,
                                  "Use this project's own cache tree from now on") };
  auto const shared_flag{ sub.flag("--shared",
                                   c.want_shared,
                                   "Use the user-wide cache from now on") };

  // One action per invocation: combining them would have to pick an order, and there is
  // no sensible one.
  root_flag.excludes(local_flag).excludes(shared_flag).excludes(user_wide_flag);
  user_wide_flag.excludes(local_flag).excludes(shared_flag);
  local_flag.excludes(shared_flag);

  sub.finalize(
      [](void *p) -> char const * {
        auto &sel{ *static_cast<cfg *>(p) };
        if (sel.want_root) { sel.act = cfg::action::PRINT_ROOT; }
        if (sel.want_user_wide) { sel.act = cfg::action::PRINT_USER_WIDE_ROOT; }
        if (sel.want_local) { sel.act = cfg::action::SET_LOCAL; }
        if (sel.want_shared) { sel.act = cfg::action::SET_SHARED; }
        return nullptr;
      },
      &c);
  return sub;
}

cmd_cache::cmd_cache(cmd_cache::cfg cfg,
                     std::optional<std::filesystem::path> const &cli_cache_root)
    : cfg_{ std::move(cfg) }, cli_cache_root_{ cli_cache_root } {}

void cmd_cache::set_mode(envy_meta const &meta,
                         std::filesystem::path const &manifest_dir,
                         cache_mode requested) {
  if (manifest_dir.empty()) {
    throw std::runtime_error(
        "cache: no envy.lua found, so there is no project to record a cache mode for");
  }

  auto const state{ resolve_state_dir(meta.state_dir, manifest_dir) };

  // Only to report what the root *was*, so a failure to determine it must not stop the
  // write. Both markers present is exactly that case: it throws, and this command -- the
  // one that owns the markers -- is the one best placed to normalize it, so it must not be
  // the one blocked by it. `--root` and the report still surface the error.
  auto const before{ [&]() -> std::optional<std::filesystem::path> {
    try {
      return resolve_cache_root(meta.cache_request(std::nullopt, manifest_dir)).root;
    } catch (std::exception const &) { return std::nullopt; }
  }() };

  // What the project itself asks for, markers ignored. Matching it means the user is
  // clearing an override rather than setting one.
  auto const declared{ resolve_cache_mode(false,
                                          false,
                                          meta.declared_cache_mode,
                                          meta.cache_local.has_value()) };

  std::filesystem::create_directories(*state);
  auto const local_marker{ *state / kCacheLocalMarker };
  auto const shared_marker{ *state / kCacheSharedMarker };

  // Throwing overload: it already returns false rather than throwing for a file that is
  // not there, so the only thing it reports is a real failure. Swallowing that left the
  // old marker in place while the command claimed the new mode -- and clearing one of the
  // two but not the other produces the both-markers state envy treats as an error.
  std::filesystem::remove(local_marker);
  std::filesystem::remove(shared_marker);

  if (requested != declared) {
    util_write_file(requested == cache_mode::LOCAL ? local_marker : shared_marker, "");
  }

  auto const after{ resolve_cache_root(meta.cache_request(std::nullopt, manifest_dir)) };
  tui::print_stdout("Cache: %s  (%s)\n",
                    after.root.string().c_str(),
                    cache_root_tier_name(after.tier));

  if (before && *before != after.root) {
    // Named, not moved: relocating a multi-GB tree across filesystems is slow and races
    // any other envy process holding a cache lock. Deleting it is the user's call.
    tui::print_stdout("Previous: %s (no longer used; remove it when convenient)\n",
                      before->string().c_str());
  }
}

void cmd_cache::execute() {
  // The report is about the project's cache, so the manifest's cache directives count here
  // exactly as they do for every other command -- read from the manifest's text, never by
  // running its Lua: a disk-usage report must not execute a project.
  //
  // --local/--shared write into the project, so they discover it whatever else is set.
  // The reporting actions skip discovery under an override, because the override already
  // decides the answer and both discovery and directive parsing throw: reading a manifest
  // that cannot change the result would turn any broken envy.lua in an ancestor directory
  // into a failed report.
  bool const writes_marker{ cfg_.act == cfg::action::SET_LOCAL ||
                            cfg_.act == cfg::action::SET_SHARED };

  // Manifest-blind by construction, and answered before discovery so the parity oracle
  // still works inside a project whose envy.lua does not parse.
  if (cfg_.act == cfg::action::PRINT_USER_WIDE_ROOT) {
    auto const user_wide{ resolve_user_wide_cache_root(cli_cache_root_) };
    if (!user_wide) {
      throw std::runtime_error(
          std::string{ "cache: cannot determine a user-wide cache root: " } +
          platform::get_default_cache_root_env_vars() + " not set");
    }
    tui::print_stdout("%s\n", user_wide->string().c_str());
    return;
  }

  envy_meta meta;
  std::filesystem::path manifest_dir;
  if (writes_marker || !cli_cache_root_) {
    // The bin dir that invoked us decides the project, not the cwd -- so
    // `B/bin/envy cache --local` records B's choice even when standing in A.
    auto const start{ manifest::discovery_start_dir(cfg_.project_dir) };
    if (auto const found{ manifest::discover(false, start) }) {
      meta = found->meta;
      manifest_dir = found->path.parent_path();
      manifest::trace_resolved(found->path,
                               start,
                               cfg_.project_dir ? "project" : "cwd",
                               false);
    }
  }

  if (writes_marker) {
    set_mode(meta,
             manifest_dir,
             cfg_.act == cfg::action::SET_LOCAL ? cache_mode::LOCAL : cache_mode::SHARED);
    return;
  }

  auto const resolved{ resolve_cache_root(
      meta.cache_request(cli_cache_root_, manifest_dir)) };
  auto const &root{ resolved.root };

  if (cfg_.act == cfg::action::PRINT_ROOT) {
    tui::print_stdout("%s\n", root.string().c_str());
    return;
  }

  std::vector<std::filesystem::path> scan_roots;
  std::vector<row> packages, deployments, other;

  auto const add{
    [&](std::vector<row> &dst, std::string label, std::filesystem::path path) {
      dst.push_back({ std::move(label), scan_roots.size(), {}, {} });
      scan_roots.push_back(std::move(path));
    }
  };

  auto const packages_dir{ root / "packages" };
  for (auto const &identity : child_dirs(packages_dir)) {
    auto const identity_dir{ packages_dir / identity.name };
    for (auto const &variant : child_dirs(identity_dir)) {
      add(packages, identity.name + "/" + variant.name, identity_dir / variant.name);
    }
  }

  auto const envy_dir{ root / "envy" };
  for (auto const &version : child_dirs(envy_dir)) {
    add(deployments, version.name, envy_dir / version.name);
  }

  // Everything else the cache holds (specs, locks, ...) so the total reconciles.
  for (auto const &e : child_dirs(root)) {
    if (e.name != "packages" && e.name != "envy") { add(other, e.name, root / e.name); }
  }

  // Seconds on a populated cache, and no total is knowable until the walk ends: the row
  // spins on what has been counted so far.
  auto const scan_section{ tui::section_create() };
  auto const scan_start{ std::chrono::steady_clock::now() };
  auto const sizes{ platform::dir_sizes(
      scan_roots,
      0,
      [&](platform::dir_size const &running) {
        tui::section_set_content(
            scan_section,
            tui::section_frame{ .label = "[cache]",
                                .content = tui::spinner_data{
                                    .text = "scanning, " + std::to_string(running.files) +
                                            " files, " + util_format_bytes(running.bytes) +
                                            " counted",
                                    .start_time = scan_start } });
      }) };
  tui::section_delete(scan_section);  // the report below is the answer

  std::uint64_t total{ 0 };
  std::size_t label_width{ 5 };  // "TOTAL"
  std::size_t size_width{ 0 };
  for (auto *rows : { &packages, &deployments, &other }) {
    for (auto &r : *rows) {
      r.size = sizes[r.index];
      r.size_text = util_format_bytes(r.size.bytes);
      total += r.size.bytes;
      label_width = std::max(label_width, r.label.size());
      size_width = std::max(size_width, r.size_text.size());
    }
    // Biggest first: the point of the command is finding what to reclaim.
    std::ranges::sort(*rows, [](row const &a, row const &b) {
      return (a.size.bytes != b.size.bytes) ? (a.size.bytes > b.size.bytes)
                                            : (a.label < b.label);
    });
  }

  auto const total_text{ util_format_bytes(total) };
  size_width = std::max(size_width, total_text.size());

  auto const lw{ static_cast<int>(label_width) };
  auto const sw{ static_cast<int>(size_width) };

  tui::print_stdout("Cache: %s  (%s)\n",
                    root.string().c_str(),
                    cache_root_tier_name(resolved.tier));
  print_section("Packages:", packages, lw, sw);
  print_section("Envy deployments:", deployments, lw, sw);
  print_section("Other:", other, lw, sw);
  tui::print_stdout("\n  %-*s  %*s\n", lw, "TOTAL", sw, total_text.c_str());
}

}  // namespace envy
