#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace envy {

struct depot_entry {
  std::string url;
  std::optional<std::string> sha256;  // lowercase 64-char hex, or nullopt
};

// Index of pre-built package archives available from remote depots.
// All depot sources merge into one flat map: package-option-hash uniqueness
// means a cache key denotes the same artifact in any depot, so source order is
// irrelevant. Duplicate keys keep the first entry; a differing SHA256 on a
// duplicate is an integrity anomaly and is warned.
class package_depot_index {
 public:
  // Build index by downloading depot manifest text files and parsing entries.
  // Failed downloads / unparseable lines are warned and skipped.
  static package_depot_index build(std::vector<std::string> const &depot_urls,
                                   std::filesystem::path const &tmp_dir);

  // Build index from pre-fetched manifest content strings (for testing).
  static package_depot_index build_from_contents(
      std::vector<std::string> const &manifest_contents);

  // Build index from a single depot manifest text. When require_sha256 is
  // false, plain-URL lines are accepted (trusted local/fn-supplied sources).
  static package_depot_index build_from_text(std::string_view text, bool require_sha256);

  // Build index from explicit entries (PACKAGE_DEPOTS FETCH table form).
  // Throws on empty url or malformed sha256; non-.tar.zst or unparseable
  // filenames are warned and skipped (parity with text parsing).
  static package_depot_index build_from_entries(std::vector<depot_entry> const &entries);

  // Build index from a local directory of .tar.zst archives.
  // Scans dir for .tar.zst files, maps their depot keys → absolute file paths.
  static package_depot_index build_from_directory(std::filesystem::path const &dir);

  // Build from directory with accompanying checksums (filename → sha256 hex).
  static package_depot_index build_from_directory(
      std::filesystem::path const &dir,
      std::unordered_map<std::string, std::string> const &checksums);

  // Merge another index into this one. Duplicate keys keep this index's entry;
  // a differing SHA256 is warned.
  void merge(package_depot_index other);

  // Returns depot entry if an exact match is found.
  std::optional<depot_entry> find(std::string_view identity,
                                  std::string_view platform,
                                  std::string_view arch,
                                  std::string_view hash_prefix) const;

  bool empty() const;

 private:
  // cache::key of the archive (label-free) → depot entry (URL + optional SHA256)
  std::unordered_map<std::string, depot_entry> entries_;
};

}  // namespace envy
