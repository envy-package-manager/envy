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
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

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

// Recreate one payload entry under `dest`. The walk is sorted, so a directory lands
// before anything inside it.
void materialize(fs::path const &src_root, fs::path const &dest, tree_entry const &e) {
  fs::path const from{ src_root / fs::path{ e.relpath } };
  fs::path const to{ dest / fs::path{ e.relpath } };

  std::error_code ec;
  switch (e.kind) {
    case tree_entry_kind::DIRECTORY: fs::create_directories(to, ec); break;
    case tree_entry_kind::SYMLINK:
      // A selected file under an unselected parent has no directory entry of its own,
      // so every kind creates its parent.
      fs::create_directories(to.parent_path(), ec);
      ec.clear();
      fs::create_symlink(e.symlink_target, to, ec);
      break;
    case tree_entry_kind::FILE:
      fs::create_directories(to.parent_path(), ec);
      ec.clear();
      fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
      break;
  }

  if (ec) {
    throw std::runtime_error("vendor: cannot write " + to.string() + ": " + ec.message());
  }
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
  std::string const label{ "[" + p->cfg->identity + "]" };
  fs::path const &dest{ destination->dir };
  vendor_validate_destination(dest, plan->project_root, p->cfg->identity);
  auto const &filter{ p->vendor_filter };

  spin(p, label, "hashing payload...");
  auto const pristine{ vendor_pristine_hash(p->pkg_path, filter) };

  auto const chosen{ [&] {
    std::error_code ec;
    if (!fs::is_directory(dest, ec) || ec) { return kAbsent; }

    // Hashed whole, with no selectors: whatever sits in the destination counts, which
    // is what makes a stray file a mismatch. The payload's own digest is the only thing
    // worth comparing against -- an edited copy and a moved-on package are both just
    // "this is not what the package holds", and both want the same repair.
    spin(p, label, "hashing vendor copy...");
    auto const digest{ tree_hash(dest).digest };
    auto const current{ util_bytes_to_hex(digest.data(), digest.size()) };
    if (current == pristine) { return kCurrent; }
    return destination->auto_sync ? kMismatch : kKept;
  }() };
  auto const &[kind, action, reason]{ chosen };

  std::uint64_t files{ 0 }, bytes{ 0 };

  if (kind == outcome::KEPT) {
    // Exempted by `auto_sync = false`: say so and leave it. A warning, not a log line --
    // the project asked to own this directory, and it is now out of step with the
    // package it came from.
    tui::warn(
        "vendored copy at %s no longer matches the package; left as it is "
        "(vendor.auto_sync = false)",
        dest.string().c_str());
    bar(p, label, 100.0, "kept: contents differ from the package", true);
  } else if (kind != outcome::CURRENT) {
    spin(p, label, "vendoring...");
    auto const entries{ tree_list(p->pkg_path, filter) };

    // Wipe rather than merge: a file the payload dropped must not survive, and
    // reconciling two trees is the same walk plus a way to get it wrong.
    if (auto const ec{ platform::remove_all_with_retry(dest) }) {
      throw std::runtime_error("vendor: cannot clear " + dest.string() + ": " +
                               ec.message());
    }
    std::error_code mk;
    fs::create_directories(dest, mk);
    if (mk) {
      throw std::runtime_error("vendor: cannot create " + dest.string() + ": " +
                               mk.message());
    }

    // Count files, not entries: directories are free to make, and the file count is what
    // the row's last word reports.
    auto const total{ std::ranges::count_if(entries, [](tree_entry const &e) {
      return e.kind == tree_entry_kind::FILE;
    }) };
    for (auto const &e : entries) {
      materialize(p->pkg_path, dest, e);
      if (e.kind != tree_entry_kind::FILE) { continue; }
      ++files;
      bytes += e.size;
      bar(p,
          label,
          100.0 * static_cast<double>(files) / static_cast<double>(total),
          std::to_string(files) + "/" + std::to_string(total) + " files",
          false);
    }

    // The last word on the row names the cause: a wipe-and-recopy over someone's edited
    // tree is worth more than a file count.
    std::string const summary{ kind == outcome::REVENDORED
                                   ? "re-vendored " + std::to_string(files) +
                                         " files: contents were dirty"
                                   : "vendored " + std::to_string(files) + " files" };
    bar(p, label, 100.0, summary, true);
    tui::debug("%s to %s", summary.c_str(), dest.string().c_str());
  } else {
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
             .files = static_cast<std::int64_t>(files),
             .bytes = static_cast<std::int64_t>(bytes),
             .duration_ms = static_cast<std::int64_t>(duration_ms));
}

}  // namespace envy
