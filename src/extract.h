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

  // Globs over extract_all_archives' filenames: a match unpacks whatever its extension, a
  // '!' match is copied whole. Includes must each match a file; extract() ignores these.
  std::vector<std::string> archives;

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
#endif

}  // namespace envy
