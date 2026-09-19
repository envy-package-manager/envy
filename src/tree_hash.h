#pragma once

#include "blake3_util.h"
#include "file_read.h"
#include "glob.h"
#include "util.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace envy {

using tree_filter = glob_filter;  // same filter archive extraction uses

struct tree_hash_result {
  blake3_t digest{};
  std::uint64_t files{ 0 };
  std::uint64_t bytes{ 0 };
};

// Filled only when a caller asks: the timers cost two clock reads per file. Stage times
// are summed across workers, so they exceed wall time; read the ratio, not the total.
struct tree_hash_stats {
  std::uint64_t dirs{ 0 };
  std::uint64_t scan_ns{ 0 };  // enumerating directories
  std::uint64_t read_ns{ 0 };  // open/read/close, excluding the hashing below
  std::uint64_t hash_ns{ 0 };  // BLAKE3 over file contents
  std::uint64_t fold_ns{ 0 };  // sorting entries and folding them into one digest
  std::uint64_t wait_ns{ 0 };  // blocked on the work queue
  std::uint64_t wall_ns{ 0 };
  unsigned threads{ 0 };
  std::vector<std::uint64_t> files_per_worker;  // the balance report: spread is the point
  std::vector<std::uint64_t> bytes_per_worker;
};

enum class tree_entry_kind : std::uint8_t { FILE, DIRECTORY, SYMLINK };

struct tree_entry {
  std::string relpath;  // '/'-joined, relative to the root
  tree_entry_kind kind{ tree_entry_kind::FILE };
  bool executable{ false };    // always false on Windows
  std::uint64_t size{ 0 };     // FILE only
  std::string symlink_target;  // SYMLINK only, as stored
};

// Folded over (relpath, kind, exec bit, content) in sorted order, so thread count never
// changes the answer. Directories fold in; symlinks hash as their target, unfollowed.
//
// An unreadable entry throws rather than call a tree it never read unchanged.
//
// `entries` takes the sorted listing the fold ran over, which the walk built either way:
// a caller about to act on this tree (the vendor phase, which wipes what it just hashed)
// then needs no second walk to find out what is in it.
tree_hash_result tree_hash(std::filesystem::path const &root,
                           tree_filter const &filter = {},
                           unsigned threads = 0,
                           tree_hash_stats *stats = nullptr,
                           std::vector<tree_entry> *entries = nullptr);

// Sorted by relpath. Same walk and same strictness as tree_hash, so a copy moves exactly
// the set the digest covered.
std::vector<tree_entry> tree_list(std::filesystem::path const &root,
                                  tree_filter const &filter = {},
                                  unsigned threads = 0);

// Per-platform traversal hooks; not called directly. Traversal stays in the OS path
// encoding and converts only at the public boundary, as platform.h's dir_scan_* hooks do.

using tree_scan_string = file_native_string;

struct tree_scan_entry {  // one child, as the platform reports it
  tree_scan_string name;
  tree_entry_kind kind{ tree_entry_kind::FILE };
  bool executable{ false };
  std::uint64_t size{ 0 };
};

// Adapt `root` for native traversal; on Windows this adds the \\?\ prefix.
tree_scan_string tree_scan_root(std::filesystem::path const &root);

// `dir` joined with one child name, in the native encoding.
tree_scan_string tree_scan_join(tree_scan_string const &dir, tree_scan_string const &name);

// The native name as UTF-8, for the relpath the digest folds over.
std::string tree_scan_utf8(tree_scan_string const &name);

// Every child of `dir`. `out` is caller-owned scratch, overwritten in place rather than
// cleared so the name strings keep their capacity. Throws naming `dir` if unreadable.
void tree_scan_one(tree_scan_string const &dir, std::vector<tree_scan_entry> &out);

// The stored target of a symlink. Throws if it cannot be read.
std::string tree_scan_link_target(tree_scan_string const &path);

// The cores worth scheduling on, not hardware_concurrency: on a 6P+6E Apple M-series
// 12 threads ran 3x slower than 6. See docs/tree-hash.md.
unsigned tree_hash_default_threads();

// Divides the machine by the calls already in flight, counting hashes and copies alike:
// every package worker is its own thread, so concurrent vendor phases must not each
// claim all of it. A non-zero request is honored as asked.
class thread_budget : unmovable {
 public:
  explicit thread_budget(unsigned requested)
      : requested_{ requested }, inflight_{ ++s_inflight } {}
  ~thread_budget() { --s_inflight; }

  unsigned threads() const {
    if (requested_) { return requested_; }
    return std::max(
        1u,
        tree_hash_default_threads() / static_cast<unsigned>(std::max(1, inflight_)));
  }

 private:
  static inline std::atomic<int> s_inflight{ 0 };
  unsigned requested_;
  int inflight_;
};

}  // namespace envy
