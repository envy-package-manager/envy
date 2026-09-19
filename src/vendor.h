#pragma once

#include "pkg_key.h"
#include "tree_hash.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace envy {

// One PACKAGES entry that asked to be vendored. No override means derive a name.
struct vendor_request {
  pkg_key key;
  std::optional<std::string> path_override;
  bool auto_sync{ true };  // false: report a mismatch, do not repair it
};

struct vendor_destination {
  std::filesystem::path dir;  // absolute, lexically normal
  bool overridden{ false };   // whether `vendor = "..."` named it
  bool auto_sync{ true };     // false: report a mismatch, do not repair it
};

// Every destination, proven collision-free. Resolved before the engine runs, so a bad
// manifest fails with nothing written.
struct vendor_plan {
  std::unordered_map<pkg_key, vendor_destination> dirs;

  // What destinations are checked against before anything is wiped.
  std::filesystem::path project_root;

  // `envy vendor --dry-run`: decide as usual, report the decision, write nothing to
  // the destination. Not "no writes at all" -- the ladder still runs, so an uncached
  // package is still fetched and installed; the vendor step is what holds off.
  bool dry_run{ false };

  // Copy workers; 0 divides the performance cores by the vendor phases in flight. A
  // knob for tools/bench_vendor.py, exactly as `envy hash --tree --threads` is one.
  unsigned threads{ 0 };

  bool empty() const { return dirs.empty(); }
  vendor_destination const *find(pkg_key const &key) const {
    auto const it{ dirs.find(key) };
    return it == dirs.end() ? nullptr : &it->second;
  }
};

// A derived name escalates only as far as it must to stay unique: name, namespace.name,
// namespace.name@revision, then an options hash. Overrides are fixed and win.
//
// Throws naming the packages on a missing VENDOR_ROOT, a path outside the project, or
// destinations that are equal or nested. Touches no filesystem.
vendor_plan vendor_resolve(std::vector<vendor_request> const &requests,
                           std::optional<std::string> const &vendor_root,
                           std::filesystem::path const &project_root);

// vendor_resolve checks the path as written; this checks where it resolves to, since a
// symlinked component would put the wipe-and-recopy outside the project.
void vendor_validate_destination(std::filesystem::path const &dest,
                                 std::filesystem::path const &project_root,
                                 std::string_view identity);

// A leading '!' excludes; an empty list selects the whole install directory. Throws
// naming `context` on an unusable pattern.
tree_filter vendor_parse_selectors(std::vector<std::string> const &raw,
                                   std::string_view context);

// Delete `entries` under `root`, then `root` itself, across `threads` workers. The
// caller passes the listing the drift check already produced, so the wipe walks nothing.
//
// Returns false when anything did not come away -- a stray file that appeared since the
// listing, a read-only file on Windows, a handle an antivirus still holds -- which is
// the caller's cue to fall back to platform::remove_all_with_retry, where the behavior
// those need already lives. A partial delete is fine: the fallback finishes the job.
bool vendor_remove_listed(std::filesystem::path const &root,
                          std::vector<tree_entry> const &entries,
                          unsigned threads);

// A stable serialization, so a cache entry's pristine hash can be keyed on the selector
// set that produced it.
std::string vendor_filter_key(tree_filter const &filter);

// Read from the cache entry if stamped there, else computed and stamped now (temp+rename,
// so concurrent backfills are safe). Keyed on the selector set, which no cache key covers.
std::string vendor_pristine_hash(std::filesystem::path const &pkg_path,
                                 tree_filter const &filter);

}  // namespace envy
