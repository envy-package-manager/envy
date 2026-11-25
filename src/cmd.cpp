#include "cmd.h"

#include "cache.h"
#include "manifest.h"
#include "platform.h"
#include "reexec.h"
#include "self_deploy.h"
#include "tui.h"
#include "util.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <io.h>
#define ENVY_DUP _dup
#define ENVY_DUP2 _dup2
#define ENVY_CLOSE _close
#define ENVY_WRITE _write
#define ENVY_FILENO _fileno
#else
#include <unistd.h>
#define ENVY_DUP ::dup
#define ENVY_DUP2 ::dup2
#define ENVY_CLOSE ::close
#define ENVY_WRITE ::write
#define ENVY_FILENO ::fileno
#endif

namespace envy {

// fd 1 points at stderr even when the caller closed it: else the next file envy opens
// lands on fd 1. restore() closes it again, so the caller's closed stdout stays closed.
stdout_to_stderr::stdout_to_stderr()
    : saved_{ (std::fflush(stdout), ENVY_DUP(ENVY_FILENO(stdout))) } {
  ENVY_DUP2(ENVY_FILENO(stderr), ENVY_FILENO(stdout));
}

stdout_to_stderr::~stdout_to_stderr() {
  if (saved_ >= 0) { ENVY_CLOSE(saved_); }
}

void stdout_to_stderr::restore() {
  std::fflush(stdout);
  if (saved_ >= 0) {
    ENVY_DUP2(saved_, ENVY_FILENO(stdout));
  } else {
    ENVY_CLOSE(ENVY_FILENO(stdout));
  }
}

void stdout_to_stderr::write(std::string_view s) const {
  if (saved_ < 0 && !s.empty()) { throw_runtime_error({ "stdout is closed" }); }
  while (!s.empty()) {
    auto const n{ ENVY_WRITE(saved_, s.data(), static_cast<unsigned>(s.size())) };
    if (n <= 0) { throw_runtime_error({ "failed to write stdout" }); }
    s.remove_prefix(static_cast<size_t>(n));
  }
}

void cmd_print_sizes(char const *label, std::vector<cmd_size_section> const &sections) {
  auto const roots{ [&] {
    std::vector<std::filesystem::path> r;
    for (auto const &s : sections) {
      for (auto const &row : s.rows) {
        if (!row.path.empty()) { r.push_back(row.path); }
      }
    }
    return r;
  }() };
  auto const spinner{ tui::section_create() };
  auto const start{ std::chrono::steady_clock::now() };
  auto const sizes{ platform::dir_sizes(roots, 0, [&](platform::dir_size const &running) {
    tui::section_set_content(
        spinner,
        tui::section_frame{ .label = label,
                            .content = tui::spinner_data{
                                .text = "scanning, " + std::to_string(running.files) +
                                        " files, " + util_format_bytes(running.bytes) +
                                        " counted",
                                .start_time = start } });
  }) };
  tui::section_delete(spinner);  // the report replaces it

  struct sized {
    cmd_size_row const *row;
    std::uint64_t bytes;
    std::string text;
  };
  std::vector<std::vector<sized>> printed;
  std::uint64_t total{ 0 };
  std::size_t label_width{ 5 };  // "TOTAL"
  std::size_t size_width{ 0 };
  std::size_t next{ 0 };
  for (auto const &s : sections) {
    auto &rows{ printed.emplace_back() };
    for (auto const &row : s.rows) {
      auto const bytes{ row.path.empty() ? row.bytes : sizes[next++].bytes };
      rows.push_back({ &row, bytes, util_format_bytes(bytes) });
      total += bytes;
      label_width = std::max(label_width, row.label.size());
      size_width = std::max(size_width, rows.back().text.size());
    }
    util_sort(rows, [](sized const &a, sized const &b) {  // largest first: what to reclaim
      return (a.bytes != b.bytes) ? (a.bytes > b.bytes) : (a.row->label < b.row->label);
    });
  }
  auto const total_text{ util_format_bytes(total) };
  size_width = std::max(size_width, total_text.size());

  auto const lw{ static_cast<int>(label_width) };
  auto const sw{ static_cast<int>(size_width) };
  for (std::size_t i{ 0 }; i < sections.size(); ++i) {
    if (sections[i].title) {
      tui::print_stdout("\n%s\n", sections[i].title);
    } else {
      tui::print_stdout("\n");
    }
    if (printed[i].empty()) { tui::print_stdout("  (none)\n"); }
    for (auto const &r : printed[i]) {
      tui::print_stdout("  %-*s  %*s%s\n",
                        lw,
                        r.row->label.c_str(),
                        sw,
                        r.text.c_str(),
                        r.row->note.c_str());
    }
  }
  tui::print_stdout("\n  %-*s  %*s\n", lw, "TOTAL", sw, total_text.c_str());
}

cmd_startup cmd_startup_load(std::string_view cmd_name,
                             std::optional<std::filesystem::path> const &manifest_path,
                             std::optional<std::filesystem::path> const &cli_cache_root,
                             bool subproject,
                             std::optional<std::filesystem::path> const &project_dir,
                             bool read_only_cache) {
  // Bytes and '@envy' header only: everything below picks the cache, and the manifest
  // may fetch a bundle into it during its own global scope (envy.loadenv_bundle).
  auto const found{ manifest::find_and_discover(manifest_path, subproject, project_dir) };
  auto const manifest_dir{ found.path.parent_path() };

  // Resolved once here and threaded onward: reexec's fast path, self-deploy and every
  // command downstream must agree, and re-resolving per consumer let a concurrent
  // `envy cache --local` split one run across two trees.
  auto const resolved{ resolve_cache_root(
      found.meta.cache_request(cli_cache_root, manifest_dir)) };

  // Throws reexec_request when it re-execs, so everything below runs only on the proceed
  // path -- which is what keeps the first-run notice from printing twice per invocation.
  reexec_if_needed(found.meta, resolved, manifest_dir);

  if (!read_only_cache) { cache_announce_root_once(resolved, found.meta.bin); }

  auto c{ self_deploy::ensure(resolved.root, resolved.mode) };
  c->set_read_only(read_only_cache);

  auto m{ manifest::load(found.content, found.path, c.get()) };
  if (!m) { throw_runtime_error({ cmd_name, ": could not load manifest" }); }
  return { std::move(m), std::move(c) };
}

}  // namespace envy
