#include "cmd_cache.h"

#include "cache.h"
#include "manifest.h"
#include "platform.h"
#include "tui.h"
#include "util.h"

#include "CLI11.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
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
  std::sort(entries.begin(),
            entries.end(),
            [](platform::dir_entry const &a, platform::dir_entry const &b) {
              return a.name < b.name;
            });
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

void cmd_cache::register_cli(CLI::App &app, std::function<void(cfg)> on_selected) {
  auto *sub{ app.add_subcommand("cache", "Show cache location and disk usage") };
  sub->callback([on_selected = std::move(on_selected)] { on_selected(cfg{}); });
}

cmd_cache::cmd_cache(cmd_cache::cfg /*cfg*/,
                     std::optional<std::filesystem::path> const &cli_cache_root)
    : cli_cache_root_{ cli_cache_root } {}

void cmd_cache::execute() {
  // The report is about the project's cache, so an '@envy cache-*' directive counts here
  // exactly as it does for every other command -- read from the manifest's text, never by
  // running its Lua: a disk-usage report must not execute a project.  Skipped entirely
  // when an override already decides, so a malformed manifest somewhere above the cwd
  // cannot break `envy cache --cache-root`; no manifest at all leaves the default tier.
  std::optional<std::string> manifest_cache;
  std::filesystem::path manifest_dir;
  if (!cli_cache_root_) {
    if (auto const found{ manifest::discover(false, std::filesystem::current_path()) }) {
      manifest_cache = found->meta.cache_for_platform();
      manifest_dir = found->path.parent_path();
    }
  }

  auto const root{ resolve_cache_root(cli_cache_root_, manifest_cache, manifest_dir) };

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

  // A recursive walk of every cache tree: seconds on a populated cache, and the total is
  // unknowable until it ends, so the row spins on what has been counted so far.
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
    std::sort(rows->begin(), rows->end(), [](row const &a, row const &b) {
      return (a.size.bytes != b.size.bytes) ? (a.size.bytes > b.size.bytes)
                                            : (a.label < b.label);
    });
  }

  auto const total_text{ util_format_bytes(total) };
  size_width = std::max(size_width, total_text.size());

  auto const lw{ static_cast<int>(label_width) };
  auto const sw{ static_cast<int>(size_width) };

  tui::print_stdout("Cache: %s\n", root.string().c_str());
  print_section("Packages:", packages, lw, sw);
  print_section("Envy deployments:", deployments, lw, sw);
  print_section("Other:", other, lw, sw);
  tui::print_stdout("\n  %-*s  %*s\n", lw, "TOTAL", sw, total_text.c_str());
}

}  // namespace envy
