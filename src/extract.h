#pragma once

#include "tui.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace envy {

struct extract_progress {
  std::uint64_t bytes_processed{ 0 };
  std::optional<std::uint64_t> total_bytes;
  std::uint64_t files_processed{ 0 };
  std::optional<std::uint64_t> total_files;
  std::filesystem::path current_entry;
  bool is_regular_file{ false };
};

using extract_progress_cb_t = std::function<bool(extract_progress const &)>;

struct extract_totals {
  std::uint64_t bytes{ 0 };
  std::uint64_t files{ 0 };
  std::vector<std::string> unmatched_selectors;  // selectors that matched nothing
};

struct extract_options {
  int strip_components{ 0 };

  // Archive-relative paths or glob patterns naming the only entries to extract, matched
  // after strip_components: a match takes that entry, or its whole subtree when it names
  // a directory. Empty extracts everything. Spelled "only" in Lua and on the CLI.
  std::vector<std::string> selectors;

  // Throw when a selector matched nothing. Callers spreading one selector list across
  // several archives clear this and validate the union themselves.
  bool require_all_selectors{ true };

  extract_progress_cb_t progress;
};

// Extract a single archive to destination
std::uint64_t extract(std::filesystem::path const &archive_path,
                      std::filesystem::path const &destination,
                      extract_options const &options = {});

// Check if path has archive extension
bool extract_is_archive_extension(std::filesystem::path const &path);

// True if an archive entry path is safe to materialize under a destination root:
// non-empty, relative, no ".." components (and no drive letter on Windows).
bool extract_is_safe_archive_path(char const *path);

// If path has a single-stream compression suffix (.gz, .bz2, .xz, .zst, .lzma) AND
// the stem is not a tar wrapper (e.g., foo.tar.gz), returns the filename with the
// suffix stripped (e.g., bar.txt.gz -> bar.txt). Otherwise returns nullopt.
std::optional<std::filesystem::path> extract_bare_compressed_output_name(
    std::filesystem::path const &archive_path);

// Create tar.zst archive from source_dir contents, stored under prefix/ (e.g., "pkg/").
// Returns number of files archived. Optional progress callback invoked per-header and
// per-chunk.
std::uint64_t archive_create_tar_zst(std::filesystem::path const &output_path,
                                     std::filesystem::path const &source_dir,
                                     std::string const &prefix,
                                     extract_progress_cb_t const &progress = {});

// Extract archives in fetch_dir to dest_dir; loose files are copied. Selectors span the
// set as a union, progress/require_all_selectors ignored, kInvalidSection = silent.
void extract_all_archives(std::filesystem::path const &fetch_dir,
                          std::filesystem::path const &dest_dir,
                          extract_options const &options,
                          std::string const &pkg_identity,
                          tui::section_handle section);

// Pre-scan one archive for the file count and uncompressed bytes options would extract.
extract_totals compute_archive_totals(std::filesystem::path const &archive_path,
                                      extract_options const &options = {});

#ifdef ENVY_UNIT_TEST
// Exposed for unit tests only - computes totals by scanning archives in a directory
extract_totals compute_extract_totals(std::filesystem::path const &fetch_dir,
                                      extract_options const &options = {});

// Selector matching, exposed for unit tests only - extract.cpp declares these for itself.
// Canonical form for matching: '\' to '/', repeated '/' collapsed, leading "./" and
// trailing '/' removed. Applied to selectors and archive paths alike.
std::string extract_canonical_match_path(std::string_view path);

// Canonicalize selectors and reject the unusable ones: empty, absolute, "..", malformed
// glob (unterminated '[', '**' sharing a component). context prefixes errors.
std::vector<std::string> extract_normalize_selectors(
    std::vector<std::string> const &selectors,
    std::string_view context);

// True when canonical glob pattern matches canonical entry_path. '*' (any run) and '?'
// (one char) stay inside one component, '**' spans components, '[a-z]'/'[!a-z]' are
// classes; matching a directory takes everything under it. Literal patterns just compare.
bool extract_glob_match(std::string_view pattern, std::string_view entry_path);

// True when any selector names canonical entry_path, globs included. Flags every selector
// that matched, so callers can report the ones that never hit anything.
bool extract_selectors_match(std::vector<std::string> const &selectors,
                             std::string_view entry_path,
                             std::vector<bool> &matched);

// Selectors never flagged in matched, in declaration order.
std::vector<std::string> extract_unmatched_selectors(
    std::vector<std::string> const &selectors,
    std::vector<bool> const &matched);
#endif

}  // namespace envy
