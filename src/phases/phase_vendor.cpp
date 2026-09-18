#include "phase_vendor.h"

#include "engine.h"
#include "pkg.h"
#include "platform.h"
#include "trace.h"
#include "tree_hash.h"
#include "tui.h"
#include "util.h"
#include "vendor.h"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace envy {

namespace {

namespace fs = std::filesystem;

// Goes verbatim into the vendor_result trace, so a test can tell "nothing was there"
// from "somebody edited it" without parsing prose.
struct verdict {
  char const *action;
  char const *reason;
  bool deploy;
};

constexpr verdict kAbsent{ "copied", "absent", true };
constexpr verdict kMismatch{ "redeployed", "mismatch", true };
constexpr verdict kCurrent{ "up_to_date", "current", false };

void draw(pkg *p, std::string const &text) {
  if (!p->tui_section) { return; }
  tui::section_set_content(
      p->tui_section,
      tui::section_frame{ .label = "[" + p->cfg->identity + "]",
                          .content = tui::spinner_data{
                              .text = text,
                              .start_time = std::chrono::steady_clock::now() } });
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

  auto const *plan{ eng.vendor_plan() };
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
  fs::path const &dest{ destination->dir };
  vendor_validate_destination(dest, plan->project_root, p->cfg->identity);
  auto const &filter{ p->vendor_filter };

  draw(p, "hashing payload...");
  auto const pristine{ vendor_pristine_hash(p->pkg_path, filter) };

  auto const [action, reason, deploy]{ [&] {
    std::error_code ec;
    if (!fs::is_directory(dest, ec) || ec) { return kAbsent; }

    // Hashed whole, with no selectors: whatever sits in the destination counts, which
    // is what makes a stray file a mismatch. The payload's own digest is the only thing
    // worth comparing against -- an edited copy and a moved-on package are both just
    // "this is not what the package holds", and both want the same repair.
    draw(p, "hashing vendor copy...");
    auto const digest{ tree_hash(dest).digest };
    auto const current{ util_bytes_to_hex(digest.data(), digest.size()) };
    return current == pristine ? kCurrent : kMismatch;
  }() };

  std::uint64_t files{ 0 }, bytes{ 0 };

  if (deploy) {
    draw(p, "vendoring...");
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

    for (auto const &e : entries) {
      materialize(p->pkg_path, dest, e);
      if (e.kind == tree_entry_kind::FILE) {
        ++files;
        bytes += e.size;
      }
    }

    tui::debug("vendored %llu file(s) to %s (%s)",
               static_cast<unsigned long long>(files),
               dest.string().c_str(),
               reason);
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
