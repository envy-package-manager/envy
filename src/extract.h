#pragma once

#include "glob.h"
#include "tui.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace envy {

// An archive's contents: its regular files and their bytes.
struct extract_totals {
  std::uint64_t files{ 0 };
  std::uint64_t bytes{ 0 };

  bool operator==(extract_totals const &) const = default;
};

// extract() fills the running counts; a tracker adds the input size and any stated totals.
struct extract_progress {
  std::uint64_t bytes_processed{ 0 };        // content bytes written
  std::uint64_t files_processed{ 0 };        // regular files begun
  std::uint64_t input_bytes_processed{ 0 };  // archive bytes read
  std::uint64_t input_bytes_total{ 0 };      // archive bytes on disk
  std::optional<extract_totals> totals;      // contents, when known before extracting
  std::filesystem::path current_entry;
  bool is_regular_file{ false };
};

using extract_progress_cb_t = std::function<bool(extract_progress const &)>;

struct extract_options {
  int strip_components{ 0 };

  // Archive-relative paths or glob patterns naming the only entries to extract, matched
  // after strip_components: a match takes that entry, or its whole subtree when it names
  // a directory. Empty extracts everything. Spelled "only" in Lua and on the CLI.
  std::vector<std::string> selectors;

  // Globs over extract_all_archives' filenames: a match unpacks whatever its extension, a
  // '!' match is copied whole. Includes must each match a file; extract() ignores these.
  std::vector<std::string> archives;

  // Null: throw when an include matched nothing. Set: OR matches in, one flag per parsed
  // include, and leave the check to a caller spreading one list across several archives.
  glob_match_flags *selector_matched{ nullptr };

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

// Create tar.zst archive from source_dir contents, stored under prefix/ (e.g., "pkg/"),
// ending in a trailer that states its totals. Returns number of files archived. Progress
// is called per-header and per-chunk, totals filled.
std::uint64_t archive_create_tar_zst(std::filesystem::path const &output_path,
                                     std::filesystem::path const &source_dir,
                                     std::string const &prefix,
                                     extract_progress_cb_t const &progress = {});

// Extract archives in fetch_dir to dest_dir; loose files are copied. Selectors span the
// set as a union, progress/selector_matched ignored, kInvalidSection = silent.
void extract_all_archives(std::filesystem::path const &fetch_dir,
                          std::filesystem::path const &dest_dir,
                          extract_options const &options,
                          std::string const &pkg_identity,
                          tui::section_handle section);

// envy's trailer on the .tar.zst it writes: a zstd skippable frame stating the contents
// (layout in docs/architecture.md). Decode accepts only what encode produces.
using extract_trailer = std::array<std::uint8_t, 32>;
extract_trailer extract_trailer_encode(extract_totals const &totals);
std::optional<extract_totals> extract_trailer_decode(extract_trailer const &bytes);

// The totals options would extract, if the archive states them without decompressing:
// envy's trailer, or a header scan where skipping an entry is a seek (plain tar, zip).
std::optional<extract_totals> extract_probe_totals(std::filesystem::path const &archive_path,
                                                   extract_options const &options = {});

}  // namespace envy
