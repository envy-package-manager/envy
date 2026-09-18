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
};

struct vendor_destination {
  std::filesystem::path dir;  // absolute, lexically normal
  bool overridden{ false };   // whether `vendor = "..."` named it
};

// Every destination, proven collision-free. Resolved before the engine runs, so a bad
// manifest fails with nothing written.
struct vendor_plan {
  std::unordered_map<pkg_key, vendor_destination> dirs;

  // One digest file per destination. Set by the caller; vendor_resolve stays pure.
  std::filesystem::path stamp_dir;

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

// A leading '!' excludes; an empty list selects the whole install directory. Throws
// naming `context` on an unusable pattern.
tree_filter vendor_parse_selectors(std::vector<std::string> const &raw,
                                   std::string_view context);

// A stable serialization, so a cache entry's pristine hash can be keyed on the selector
// set that produced it.
std::string vendor_filter_key(tree_filter const &filter);

// Read from the cache entry if stamped there, else computed and stamped now (temp+rename,
// so concurrent backfills are safe). Keyed on the selector set, which no cache key covers.
std::string vendor_pristine_hash(std::filesystem::path const &pkg_path,
                                 tree_filter const &filter);

// Named for the destination, which is what the digest describes; a package that moves
// stops consulting its old stamp.
std::filesystem::path vendor_stamp_path(std::filesystem::path const &stamp_dir,
                                        std::filesystem::path const &dest);

// The digest in `stamp`, or nullopt when it is missing or unreadable.
std::optional<std::string> vendor_read_stamp(std::filesystem::path const &stamp);

// Record `digest` as the state of `dest`. Creates `stamp_dir` if needed.
void vendor_write_stamp(std::filesystem::path const &stamp_dir,
                        std::filesystem::path const &dest,
                        std::string_view digest);

}  // namespace envy
