#include "phase_vendor.h"

#include "engine.h"
#include "pkg.h"
#include "platform.h"
#include "trace.h"
#include "tree_hash.h"
#include "tui.h"
#include "util.h"
#include "vendor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace envy {

namespace {

namespace fs = std::filesystem;

// What the phase decided. `action` and `reason` go verbatim into the vendor_result
// trace, so a test can tell these apart without parsing prose.
enum class outcome { COPIED, REVENDORED, KEPT, CURRENT };

struct verdict {
  outcome kind;
  char const *action;
  char const *reason;
};

constexpr verdict kAbsent{ outcome::COPIED, "copied", "absent" };
constexpr verdict kMismatch{ outcome::REVENDORED, "redeployed", "mismatch" };
constexpr verdict kKept{ outcome::KEPT, "kept", "mismatch" };
constexpr verdict kCurrent{ outcome::CURRENT, "up_to_date", "current" };

void spin(pkg *p, std::string const &label, std::string text) {
  if (!p->tui_section) { return; }
  tui::section_set_content(
      p->tui_section,
      tui::section_frame{ .label = label,
                          .content = tui::spinner_data{
                              .text = std::move(text),
                              .start_time = std::chrono::steady_clock::now() } });
}

// A bar, not a spinner: tree_list already counted the work. `terminal` is the row's last
// word, and lands as soon as it is set.
void bar(pkg *p,
         std::string const &label,
         double percent,
         std::string status,
         bool terminal) {
  if (!p->tui_section) { return; }
  tui::section_set_content(
      p->tui_section,
      tui::section_frame{
          .label = label,
          .content = tui::progress_data{ .percent = percent, .status = std::move(status) },
          .terminal = terminal });
}

// Recreate one payload entry under `dest`. A selection carries the ancestors of its
// entries (tree_list guarantees it), so every parent directory is an entry of its own
// and was created by the pass before this one -- nothing here creates a parent, which
// is what lets the file pass run on many threads at once.
void materialize(fs::path const &src_root, fs::path const &dest, tree_entry const &e) {
  fs::path const to{ dest / fs::path{ e.relpath } };

  std::error_code ec;
  switch (e.kind) {
    case tree_entry_kind::DIRECTORY:
      // The pass that calls this runs in sorted order, so `to`'s parent already exists:
      // one mkdir, not create_directories' stat per path component. The slow call is
      // reached only on failure, where it fails the same way and says why.
      if (platform::make_dir(to)) { return; }
      fs::create_directories(to, ec);
      break;
    case tree_entry_kind::SYMLINK: fs::create_symlink(e.symlink_target, to, ec); break;
    case tree_entry_kind::FILE: {
      fs::path const from{ src_root / fs::path{ e.relpath } };
      // Sharing the payload's blocks beats copying them by two orders of magnitude on a
      // large file, and the destination was just wiped, so nothing is in the way. False
      // means this filesystem or this pair of volumes cannot, not that anything failed.
      if (platform::clone_file(from, to)) { return; }
      fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
      break;
    }
  }

  if (ec) {
    throw std::runtime_error("vendor: cannot write " + to.string() + ": " + ec.message());
  }
}

// Copy `entries` into `dest` with `threads` workers, reporting progress as files land.
//
// Two passes, because they have different shapes: directories are created in sorted
// order on this thread (a parent before its children, which is the ordering guarantee a
// parallel pass would have to rebuild), then every file and symlink is handed to a pool.
// Per-file work is a syscall or two, so the pool takes a contiguous slice each rather
// than one entry at a time -- an index atomic per file costs more than the copy.
std::uint64_t copy_entries(fs::path const &src_root,
                           fs::path const &dest,
                           std::vector<tree_entry> const &entries,
                           unsigned threads,
                           std::function<void(std::uint64_t)> const &on_file) {
  std::vector<tree_entry const *> files;
  files.reserve(entries.size());
  for (auto const &e : entries) {
    if (e.kind == tree_entry_kind::DIRECTORY) {
      materialize(src_root, dest, e);
    } else {
      files.push_back(&e);
    }
  }

  std::atomic<std::size_t> next{ 0 };
  std::atomic<std::uint64_t> done{ 0 };
  std::mutex error_mutex;
  std::exception_ptr error;

  auto const worker{ [&] {
    constexpr std::size_t kSlice{ 32 };
    for (std::size_t first{ next.fetch_add(kSlice) }; first < files.size();
         first = next.fetch_add(kSlice)) {
      auto const last{ std::min(first + kSlice, files.size()) };
      try {
        for (std::size_t i{ first }; i < last; ++i) {
          materialize(src_root, dest, *files[i]);
          // Every worker reports; the callback draws at most one frame per slice, so a
          // 50,000-file payload does not spend its time in the renderer.
          auto const n{ done.fetch_add(1) + 1 };
          if (i + 1 == last) { on_file(n); }
        }
      } catch (...) {
        std::lock_guard const lock{ error_mutex };
        if (!error) { error = std::current_exception(); }
        next.store(files.size());  // a half-written tree is not worth finishing
        return;
      }
    }
  } };

  unsigned const n{ std::max(1u, std::min<unsigned>(threads, 1 + files.size() / 32)) };
  std::vector<std::thread> pool;
  pool.reserve(n - 1);
  for (unsigned i{ 1 }; i < n; ++i) { pool.emplace_back(worker); }
  worker();
  for (auto &t : pool) { t.join(); }

  if (error) { std::rethrow_exception(error); }
  return done.load();
}

}  // namespace

void run_vendor_phase(pkg *p, engine &eng) {
  phase_trace_scope const phase_scope{ p->cfg->identity,
                                       pkg_phase::pkg_vendor,
                                       std::chrono::steady_clock::now() };

  auto const *plan{ eng.vendor() };
  if (!plan) { return; }  // Not a vendoring run; short-circuit.

  auto const *destination{ plan->find(p->key) };
  if (!destination) { return; }  // This package was never asked for.

  // spec_fetch rejects user-managed, and a bundle entry cannot carry `vendor`, so this
  // only fires if a new package type appeared.
  if (p->type != pkg_type::CACHE_MANAGED) {
    throw std::runtime_error("vendor: " + p->cfg->identity +
                             " has no cached payload to vendor");
  }

  auto const start{ std::chrono::steady_clock::now() };
  bool const dry{ plan->dry_run };  // decide as usual, write nothing

  // Stage accounting, reported on vendor_result. Hashing, wiping and copying answer to
  // different limits -- cores, the filesystem's unlink path, and copy bandwidth -- so
  // one duration cannot say which of them a slow run was waiting on.
  std::int64_t hash_ms{ 0 }, wipe_ms{ 0 }, copy_ms{ 0 };
  auto const elapsed_ms{ [](std::chrono::steady_clock::time_point since) {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - since)
            .count());
  } };
  std::string const label{ "[" + p->cfg->identity + "]" };
  fs::path const &dest{ destination->dir };
  vendor_validate_destination(dest, plan->project_root, p->cfg->identity);
  auto const &filter{ p->vendor_filter };

  spin(p, label, "hashing payload...");
  auto const hash_start{ std::chrono::steady_clock::now() };
  auto const pristine{ vendor_pristine_hash(p->pkg_path, filter) };

  // What the drift check found in the destination. The wipe below deletes exactly this,
  // rather than walking the same tree a second time to rediscover it.
  std::vector<tree_entry> present;

  auto const chosen{ [&] {
    std::error_code ec;
    if (!fs::is_directory(dest, ec) || ec) { return kAbsent; }

    // Hashed whole, with no selectors: whatever sits in the destination counts, which
    // is what makes a stray file a mismatch. The payload's own digest is the only thing
    // worth comparing against -- an edited copy and a moved-on package are both just
    // "this is not what the package holds", and both want the same repair.
    spin(p, label, "hashing vendor copy...");
    auto const digest{ tree_hash(dest, {}, 0, nullptr, &present).digest };
    auto const current{ util_bytes_to_hex(digest.data(), digest.size()) };
    if (current == pristine) { return kCurrent; }
    return destination->auto_sync ? kMismatch : kKept;
  }() };
  hash_ms = elapsed_ms(hash_start);
  auto const &[kind, action, reason]{ chosen };

  std::uint64_t files{ 0 }, bytes{ 0 };

  if (kind == outcome::KEPT) {
    // Exempted by `auto_sync = false`: say so and leave it. A warning, not a log line --
    // the project asked to own this directory, and it is now out of step with the
    // package it came from.
    tui::warn(
        "vendored copy at %s no longer matches the package; left as it is "
        "(vendor.auto_sync = false; 'envy vendor --force' restores it)",
        dest.string().c_str());
    p->vendor_outcome = "kept " + dest.string() + ": contents differ from the package";
    bar(p, label, 100.0, "kept: contents differ from the package", true);
  } else if (kind != outcome::CURRENT) {
    spin(p, label, dry ? "counting..." : "vendoring...");
    auto const copy_start{ std::chrono::steady_clock::now() };
    auto const entries{ tree_list(p->pkg_path, filter) };

    // Wipe rather than merge: a file the payload dropped must not survive, and
    // reconciling two trees is the same walk plus a way to get it wrong.
    if (!dry) {
      auto const wipe_start{ std::chrono::steady_clock::now() };
      thread_budget const budget{ plan->threads };
      // The listed delete is the fast path and reports when it did not finish; the
      // retrying whole-tree remove is the backstop, and is the whole story for an absent
      // destination, where the drift check listed nothing.
      bool const cleared{ !present.empty() &&
                          vendor_remove_listed(dest, present, budget.threads()) };
      if (!cleared) {
        if (auto const ec{ platform::remove_all_with_retry(dest) }) {
          throw std::runtime_error("vendor: cannot clear " + dest.string() + ": " +
                                   ec.message());
        }
      }
      std::error_code mk;
      fs::create_directories(dest, mk);
      if (mk) {
        throw std::runtime_error("vendor: cannot create " + dest.string() + ": " +
                                 mk.message());
      }
      wipe_ms = elapsed_ms(wipe_start);
    }

    // Count files, not entries: directories are free to make, and the file count is what
    // the row's last word reports.
    auto const total{ std::ranges::count_if(entries, [](tree_entry const &e) {
      return e.kind == tree_entry_kind::FILE;
    }) };
    for (auto const &e : entries) {
      if (e.kind != tree_entry_kind::FILE) { continue; }
      ++files;
      bytes += e.size;
    }

    if (!dry) {
      thread_budget const budget{ plan->threads };
      copy_entries(p->pkg_path,
                   dest,
                   entries,
                   budget.threads(),
                   [&](std::uint64_t done) {
                     bar(p,
                         label,
                         100.0 * static_cast<double>(done) /
                             static_cast<double>(std::max<std::int64_t>(total, 1)),
                         std::to_string(done) + "/" + std::to_string(total) + " files",
                         false);
                   });
    }

    // The last word names the cause: a wipe-and-recopy over someone's edited tree is
    // worth more than a file count. The destination goes before it rather than after --
    // "N files: contents were dirty to <dir>" reads as if the directory were the dirt.
    bool const revendor{ kind == outcome::REVENDORED };
    std::string const counted{ (revendor ? (dry ? "would re-vendor " : "re-vendored ")
                                         : (dry ? "would vendor " : "vendored ")) +
                               std::to_string(files) + " files" };
    char const *const cause{ revendor ? ": contents were dirty" : "" };
    copy_ms = elapsed_ms(copy_start) - wipe_ms;  // the walk and the writes, not the wipe
    p->vendor_outcome = counted + " to " + dest.string() + cause;
    if (!dry) { bar(p, label, 100.0, counted + cause, true); }
    tui::debug("%s", p->vendor_outcome.c_str());
  } else {
    p->vendor_outcome = "up to date: " + dest.string();
    tui::debug("vendor copy at %s is up to date", dest.string().c_str());
  }

  auto const duration_ms{ std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count() };

  ENVY_TRACE(vendor_result,
             p->cfg->identity,
             .path = dest.generic_string(),
             .action = action,
             .reason = reason,
             .dry_run = dry,
             .files = static_cast<std::int64_t>(files),
             .bytes = static_cast<std::int64_t>(bytes),
             .hash_ms = hash_ms,
             .wipe_ms = wipe_ms,
             .copy_ms = copy_ms,
             .duration_ms = static_cast<std::int64_t>(duration_ms));
}

}  // namespace envy
