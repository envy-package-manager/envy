#include "extract.h"

#include "trace.h"
#include "tui.h"
#include "util.h"

#include "archive.h"
#include "archive_entry.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace envy {

// Selector matching lives here and in extract_tests.cpp only, so extract.h publishes it
// under ENVY_UNIT_TEST alone; these declarations serve the helpers below.
std::string extract_canonical_match_path(std::string_view path);
std::vector<std::string> extract_normalize_selectors(
    std::vector<std::string> const &selectors,
    std::string_view context);
bool extract_glob_match(std::string_view pattern, std::string_view entry_path);
bool extract_selectors_match(std::vector<std::string> const &selectors,
                             std::string_view entry_path,
                             std::vector<bool> &matched);
std::vector<std::string> extract_unmatched_selectors(
    std::vector<std::string> const &selectors,
    std::vector<bool> const &matched);

namespace {

struct archive_reader : unmovable {
  explicit archive_reader(bool enable_raw_format = false) : handle(archive_read_new()) {
    if (!handle) { throw std::runtime_error("archive_read_new failed"); }
    archive_read_support_filter_all(handle);
    archive_read_support_format_all(handle);
    // libarchive matches raw last; opt in only when we expect a bare compressed
    // stream so unknown binary blobs don't silently "extract" as a copy.
    if (enable_raw_format) { archive_read_support_format_raw(handle); }
  }

  ~archive_reader() {
    if (handle) {
      archive_read_close(handle);
      archive_read_free(handle);
    }
  }

  archive *handle{ nullptr };
};

struct archive_writer : unmovable {
  archive_writer() : handle(archive_write_disk_new()) {
    if (!handle) { throw std::runtime_error("archive_write_disk_new failed"); }
    archive_write_disk_set_options(handle,
                                   ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                       ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS |
                                       ARCHIVE_EXTRACT_SECURE_SYMLINKS |
                                       ARCHIVE_EXTRACT_SECURE_NODOTDOT);
    archive_write_disk_set_standard_lookup(handle);
  }

  ~archive_writer() {
    if (handle) {
      archive_write_close(handle);
      archive_write_free(handle);
    }
  }

  archive *handle{ nullptr };
};

void ensure_directory(std::filesystem::path const &path) {
  auto const dir{ path.parent_path() };
  if (dir.empty()) { return; }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    throw std::runtime_error(std::string("Failed to create directory ") + dir.string() +
                             ": " + ec.message());
  }
}

std::optional<std::string> strip_path_components(char const *path, int strip_count) {
  if (!path) { return std::nullopt; }
  if (strip_count <= 0) { return std::string(path); }

  char const *p{ path };
  int components_stripped{ 0 };

  while (*p == '/') { ++p; }

  while (components_stripped < strip_count) {
    if (*p == '\0') { return std::nullopt; }
    if (*p == '/') {
      ++components_stripped;
      while (*p == '/') { ++p; }
    } else {
      ++p;
    }
  }

  if (*p == '\0') { return std::nullopt; }
  return std::string(p);
}

// Build list of files to extract from fetch_dir
std::vector<std::string> collect_extract_items(std::filesystem::path const &fetch_dir) {
  std::vector<std::string> items;
  if (!std::filesystem::exists(fetch_dir)) { return items; }

  for (auto const &entry : std::filesystem::directory_iterator(fetch_dir)) {
    if (!entry.is_regular_file()) { continue; }
    if (entry.path().filename() == "envy-complete") { continue; }
    items.push_back(entry.path().filename().string());
  }
  return items;
}

// One '/'-delimited component off the front; rest is what follows the separator, empty
// once nothing is left. Canonical paths have no empty components, so "" means exhausted.
struct path_split {
  std::string_view head, rest;
};

path_split split_component(std::string_view path) {
  auto const slash{ path.find('/') };
  return slash == std::string_view::npos
             ? path_split{ path, {} }
             : path_split{ path.substr(0, slash), path.substr(slash + 1) };
}

// Match c against the '[...]' class opening at pat[open]; returns the index past ']'.
// Assumes the class is terminated - extract_normalize_selectors rejects the rest.
std::pair<std::size_t, bool> glob_class_match(std::string_view pat,
                                              std::size_t open,
                                              char c) {
  std::size_t i{ open + 1 };
  bool const negate{ i < pat.size() && (pat[i] == '!' || pat[i] == '^') };
  if (negate) { ++i; }

  bool matched{ false };
  for (bool first{ true }; i < pat.size(); ++i, first = false) {
    if (pat[i] == ']' && !first) {
      ++i;
      break;
    }
    if (i + 2 < pat.size() && pat[i + 1] == '-' && pat[i + 2] != ']') {
      if (c >= pat[i] && c <= pat[i + 2]) { matched = true; }
      i += 2;
      continue;
    }
    if (pat[i] == c) { matched = true; }
  }
  return { i, matched != negate };
}

// Match one path component: '*' any run, '?' one character, '[...]' a class, everything
// else literal. One saved star is enough to be complete, so no recursion and no allocs.
bool glob_component_match(std::string_view pat, std::string_view text) {
  constexpr auto kNone{ std::string_view::npos };
  std::size_t pi{ 0 }, ti{ 0 }, star_pi{ kNone }, star_ti{ 0 };

  while (ti < text.size()) {
    bool advanced{ false };
    if (pi < pat.size()) {
      if (pat[pi] == '*') {
        star_pi = pi++;
        star_ti = ti;
        continue;
      }
      if (pat[pi] == '?') {
        ++pi;
        ++ti;
        continue;
      }
      if (pat[pi] == '[') {
        if (auto const [next, hit]{ glob_class_match(pat, pi, text[ti]) }; hit) {
          pi = next;
          ++ti;
          advanced = true;
        }
      } else if (pat[pi] == text[ti]) {
        ++pi;
        ++ti;
        advanced = true;
      }
    }
    if (advanced) { continue; }
    if (star_pi == kNone) { return false; }
    pi = star_pi + 1;  // let the last '*' eat one more character
    ti = ++star_ti;
  }

  while (pi < pat.size() && pat[pi] == '*') { ++pi; }
  return pi == pat.size();
}

// Why a canonical selector can't be matched unambiguously, or nullopt when it can.
// Rejecting beats guessing: a malformed pattern is a typo, and typos must be loud.
std::optional<std::string_view> selector_problem(std::string_view entry) {
  for (std::string_view rest{ entry }; !rest.empty();) {
    auto const [component, tail]{ split_component(rest) };
    rest = tail;

    if (component.find("**") != std::string_view::npos && component != "**") {
      return "must give '**' a path component of its own";
    }

    for (std::size_t i{ 0 }; i < component.size(); ++i) {
      if (component[i] != '[') { continue; }
      std::size_t scan{ i + 1 };
      if (scan < component.size() && (component[scan] == '!' || component[scan] == '^')) {
        ++scan;
      }
      if (scan < component.size() && component[scan] == ']') { ++scan; }  // literal ']'
      auto const close{ component.find(']', scan) };
      if (close == std::string_view::npos) { return "has an unterminated '[' class"; }
      i = close;
    }
  }
  return std::nullopt;
}

// The path extract() would write, canonicalized for selector matching: derived name for a
// raw stream, else the stripped entry path. nullopt when strip drops the entry.
std::optional<std::string> selector_match_path(
    archive_entry *entry,
    bool is_raw_stream,
    std::optional<std::filesystem::path> const &bare_name,
    int strip_components) {
  if (is_raw_stream && bare_name) { return bare_name->generic_string(); }
  auto const stripped{ strip_path_components(archive_entry_pathname(entry),
                                             strip_components) };
  if (!stripped) { return std::nullopt; }
  return extract_canonical_match_path(*stripped);
}

// Add the file count and uncompressed bytes of the entries `selectors` selects (all when
// empty) to totals, flagging selectors that matched. context prefixes errors.
void accumulate_archive_totals(std::filesystem::path const &archive_path,
                               std::vector<std::string> const &selectors,
                               int strip_components,
                               std::string_view context,
                               extract_totals &totals,
                               std::vector<bool> &selector_matched) {
  auto const bare_name{ extract_bare_compressed_output_name(archive_path) };
  archive_reader reader{ bare_name.has_value() };
  if (archive_read_open_filename(reader.handle, archive_path.string().c_str(), 10240) !=
      ARCHIVE_OK) {
    throw std::runtime_error(std::string(context) + ": failed to open " +
                             archive_path.string() + ": " +
                             archive_error_string(reader.handle));
  }

  archive_entry *entry{ nullptr };
  while (true) {
    int const r{ archive_read_next_header(reader.handle, &entry) };
    if (r == ARCHIVE_EOF) { break; }
    if (r != ARCHIVE_OK) {
      throw std::runtime_error(std::string(context) + ": header error in " +
                               archive_path.string() + ": " +
                               archive_error_string(reader.handle));
    }
    bool const is_raw_stream{ archive_format(reader.handle) == ARCHIVE_FORMAT_RAW };
    // Mirror extract()'s validation: if the suffix promised compression but no
    // decompression filter actually matched, this is corrupt or misnamed input.
    if (is_raw_stream && bare_name &&
        archive_filter_code(reader.handle, 0) == ARCHIVE_FILTER_NONE) {
      throw std::runtime_error(std::string(context) + ": not a valid compressed stream: " +
                               archive_path.string());
    }

    if (!selectors.empty()) {
      auto const match_path{
        selector_match_path(entry, is_raw_stream, bare_name, strip_components)
      };
      if (!match_path ||
          !extract_selectors_match(selectors, *match_path, selector_matched)) {
        continue;
      }
    }

    if (!is_raw_stream && archive_entry_filetype(entry) != AE_IFREG) { continue; }
    la_int64_t const size{ archive_entry_size(entry) };
    if (size > 0) {
      totals.bytes += static_cast<std::uint64_t>(size);
    } else if (is_raw_stream) {
      // Raw entries report size==-1 (unknown). Use compressed source size as an
      // approximation for progress UI.
      std::error_code ec;
      auto const fsize{ std::filesystem::file_size(archive_path, ec) };
      if (!ec) { totals.bytes += fsize; }
    }
    ++totals.files;
  }
}

void throw_on_unmatched_selectors(std::vector<std::string> const &unmatched,
                                  std::string_view context) {
  if (unmatched.empty()) { return; }
  std::string msg{ std::string(context) +
                   ": 'only' entries matched no archive contents:" };
  for (auto const &u : unmatched) { msg += " \"" + u + "\""; }
  throw std::runtime_error(msg);
}

// TUI progress state for extract_all_archives
struct extract_tui_state {
  tui::section_handle section;
  std::string label;
  std::vector<tui::section_frame> children;
  bool grouped;
  extract_totals totals;
  std::uint64_t files_processed{ 0 };
  std::uint64_t bytes_processed{ 0 };
  std::filesystem::path last_file_seen;
  std::optional<std::size_t> current_file_idx;
  std::chrono::steady_clock::time_point start_time{ std::chrono::steady_clock::now() };

  extract_tui_state(tui::section_handle s,
                    std::string const &pkg_identity,
                    std::vector<std::string> const &filenames,
                    extract_totals const &t)
      : section{ s },
        label{ "[" + pkg_identity + "]" },
        grouped{ filenames.size() > 1 },
        totals{ t } {
    children.reserve(filenames.size());
    for (auto const &name : filenames) {
      children.push_back(
          tui::section_frame{ .label = name,
                              .content = tui::static_text_data{ .text = "pending" } });
    }
  }

  void set_spinner(std::string const &text) {
    tui::section_set_content(
        section,
        tui::section_frame{ .label = label,
                            .content = tui::spinner_data{
                                .text = text,
                                .start_time = std::chrono::steady_clock::now() } });
  }

  void update_progress(bool terminal = false) {
    bool const known{ totals.files > 0 || totals.bytes > 0 };
    // Clamped by hand: archive.h drags in windows.h without NOMINMAX, so `min` is a macro.
    double const percent{ [&]() -> double {
      if (terminal) { return 100.0; }
      double raw{ 0.0 };
      if (totals.files > 0) {
        raw = (files_processed / static_cast<double>(totals.files)) * 100.0;
      } else if (totals.bytes > 0) {
        raw = (bytes_processed / static_cast<double>(totals.bytes)) * 100.0;
      }
      return raw > 100.0 ? 100.0 : raw;
    }() };

    std::ostringstream status;
    status << files_processed;
    if (totals.files > 0) { status << "/" << totals.files; }
    status << " files";
    if (totals.bytes > 0) {
      status << " " << util_format_bytes(bytes_processed) << "/"
             << util_format_bytes(totals.bytes);
    } else if (bytes_processed > 0) {
      status << " " << util_format_bytes(bytes_processed);
    }

    std::string const item{ children.empty() ? "" : children.front().label };
    std::string const text{ (grouped || item.empty()) ? status.str()
                                                      : status.str() + " " + item };

    // The pre-scan sized nothing, so there is nothing to divide by: spin on the running
    // counts instead of a bar stuck at 0%.
    auto const kids{ grouped ? children : std::vector<tui::section_frame>{} };
    if (!known && !terminal) {
      tui::section_set_content(
          section,
          tui::section_frame{
              .label = label,
              .content = tui::spinner_data{ .text = text, .start_time = start_time },
              .children = kids });
      return;
    }

    tui::section_set_content(
        section,
        tui::section_frame{ .label = label,
                            .content =
                                tui::progress_data{ .percent = percent, .status = text },
                            .children = kids,
                            .terminal = terminal });
  }

  void on_file_start(std::string const &name) {
    // Mark previous file as done
    if (current_file_idx && *current_file_idx < children.size()) {
      children[*current_file_idx].content = tui::static_text_data{ .text = "done" };
    }

    // Find and mark current file as in-progress
    if (auto it{ std::find_if(children.begin(),
                              children.end(),
                              [&](auto const &c) { return c.label == name; }) };
        it != children.end()) {
      auto idx{ static_cast<std::size_t>(std::distance(children.begin(), it)) };
      current_file_idx = idx;
      children[idx].content =
          tui::spinner_data{ .text = "extracting",
                             .start_time = std::chrono::steady_clock::now() };
    }
    update_progress();
  }

  bool on_progress(std::uint64_t bytes,
                   std::filesystem::path const &entry,
                   bool is_regular_file) {
    bytes_processed = bytes;
    if (is_regular_file && entry != last_file_seen) {
      ++files_processed;
      last_file_seen = entry;
    }
    update_progress();
    return true;
  }

  // Everything is out, but the counters trail the pre-scan totals (a directory entry is
  // not a counted file) and the last archive still reads in-flight.
  void finish() {
    if (current_file_idx && *current_file_idx < children.size()) {
      children[*current_file_idx].content = tui::static_text_data{ .text = "done" };
      current_file_idx.reset();
    }
    files_processed = totals.files;
    bytes_processed = totals.bytes;
    update_progress(true);
  }
};

}  // namespace

std::string extract_canonical_match_path(std::string_view path) {
  std::string collapsed;
  collapsed.reserve(path.size());
  for (char const c : path) {  // '\' is a separator, and "a//b" is "a/b"
    char const sep{ (c == '\\') ? '/' : c };
    if (sep == '/' && !collapsed.empty() && collapsed.back() == '/') { continue; }
    collapsed.push_back(sep);
  }

  std::string_view view{ collapsed };
  while (view.starts_with("./")) { view.remove_prefix(2); }
  while (!view.empty() && view.back() == '/') { view.remove_suffix(1); }
  if (view == ".") { view = {}; }  // "." names the root, which no selector can name
  return std::string{ view };
}

bool extract_glob_match(std::string_view pattern, std::string_view entry_path) {
  std::string_view star_pattern{}, star_path{};
  bool have_star{ false };

  while (true) {
    // Pattern exhausted: whatever is left of the path sits under what already matched,
    // which is how a directory entry pulls in its subtree.
    if (pattern.empty()) { return true; }

    auto const pat{ split_component(pattern) };
    if (pat.head == "**") {  // start by letting it match zero components
      have_star = true;
      star_pattern = pat.rest;
      star_path = entry_path;
      pattern = pat.rest;
      continue;
    }

    if (!entry_path.empty()) {
      if (auto const path{ split_component(entry_path) };
          glob_component_match(pat.head, path.head)) {
        pattern = pat.rest;
        entry_path = path.rest;
        continue;
      }
    }

    if (!have_star || star_path.empty()) { return false; }
    star_path = split_component(star_path).rest;  // '**' eats one more component
    pattern = star_pattern;
    entry_path = star_path;
  }
}

std::vector<std::string> extract_normalize_selectors(
    std::vector<std::string> const &selectors,
    std::string_view context) {
  std::vector<std::string> normalized;
  normalized.reserve(selectors.size());
  for (auto const &raw : selectors) {
    std::string canonical{ extract_canonical_match_path(raw) };
    if (canonical.empty() || !extract_is_safe_archive_path(canonical.c_str())) {
      throw std::runtime_error(std::string(context) +
                               ": 'only' entry must be a non-empty archive-relative "
                               "path without '..': \"" +
                               raw + "\"");
    }
    if (auto const problem{ selector_problem(canonical) }; problem) {
      throw std::runtime_error(std::string(context) + ": 'only' entry \"" + raw + "\" " +
                               std::string(*problem));
    }
    normalized.push_back(std::move(canonical));
  }
  return normalized;
}

bool extract_selectors_match(std::vector<std::string> const &selectors,
                             std::string_view entry_path,
                             std::vector<bool> &matched) {
  matched.resize(selectors.size(), false);
  bool selected{ false };
  for (std::size_t i{ 0 }; i < selectors.size(); ++i) {
    if (extract_glob_match(selectors[i], entry_path)) {
      matched[i] = true;
      selected = true;
    }
  }
  return selected;
}

std::vector<std::string> extract_unmatched_selectors(
    std::vector<std::string> const &selectors,
    std::vector<bool> const &matched) {
  std::vector<std::string> unmatched;
  for (std::size_t i{ 0 }; i < selectors.size(); ++i) {
    if (i >= matched.size() || !matched[i]) { unmatched.push_back(selectors[i]); }
  }
  return unmatched;
}

bool extract_is_safe_archive_path(char const *path) {
  if (!path || path[0] == '\0') { return false; }
  if (path[0] == '/' || path[0] == '\\') { return false; }
#ifdef _WIN32
  if (util_ascii_is_alpha(path[0]) && path[1] == ':') { return false; }
#endif
  std::string_view sv{ path };
  // Reject paths containing ".." components
  for (std::size_t pos{ 0 }; pos < sv.size();) {
    auto const sep{ sv.find_first_of("/\\", pos) };
    if (sv.substr(pos, sep == std::string_view::npos ? sep : sep - pos) == "..") {
      return false;
    }
    pos = (sep == std::string_view::npos) ? sv.size() : sep + 1;
  }
  return true;
}

std::uint64_t archive_create_tar_zst(std::filesystem::path const &output_path,
                                     std::filesystem::path const &source_dir,
                                     std::string const &prefix,
                                     extract_progress_cb_t const &progress) {
  archive *a{ archive_write_new() };
  if (!a) { throw std::runtime_error("archive_write_new failed"); }

  archive_write_set_format_pax_restricted(a);
  archive_write_add_filter_zstd(a);

  ensure_directory(output_path);

  if (archive_write_open_filename(a, output_path.string().c_str()) != ARCHIVE_OK) {
    std::string msg{ std::string("Failed to open output: ") + archive_error_string(a) };
    archive_write_free(a);
    throw std::runtime_error(msg);
  }

  archive_entry *entry{ archive_entry_new() };
  std::uint64_t files_archived{ 0 };
  std::uint64_t bytes_processed{ 0 };
  std::vector<char> buffer(1024 * 1024);

  for (auto const &dir_entry : std::filesystem::recursive_directory_iterator(source_dir)) {
    std::filesystem::path const rel{ dir_entry.path().lexically_relative(source_dir) };
    std::string const archived_path{ prefix + "/" + rel.generic_string() };

    archive_entry_clear(entry);
    archive_entry_set_pathname(entry, archived_path.c_str());

    bool const is_regular{ dir_entry.is_regular_file() };

    if (dir_entry.is_symlink()) {
      archive_entry_set_filetype(entry, AE_IFLNK);
      auto const target{ std::filesystem::read_symlink(dir_entry.path()) };
      archive_entry_set_symlink(entry, target.string().c_str());
      archive_entry_set_size(entry, 0);
    } else if (dir_entry.is_directory()) {
      archive_entry_set_filetype(entry, AE_IFDIR);
      archive_entry_set_size(entry, 0);
    } else if (is_regular) {
      archive_entry_set_filetype(entry, AE_IFREG);
      auto const sz{ std::filesystem::file_size(dir_entry.path()) };
      archive_entry_set_size(entry, static_cast<la_int64_t>(sz));
    } else {
      continue;
    }

    // Preserve permissions
    std::error_code ec;
    auto const perms{ std::filesystem::status(dir_entry.path(), ec).permissions() };
    if (!ec) { archive_entry_set_perm(entry, static_cast<__LA_MODE_T>(perms)); }

    if (progress) {
      progress({ .bytes_processed = bytes_processed,
                 .files_processed = files_archived,
                 .current_entry = rel,
                 .is_regular_file = is_regular });
    }

    if (archive_write_header(a, entry) != ARCHIVE_OK) {
      std::string msg{ std::string("Failed to write header: ") + archive_error_string(a) };
      archive_entry_free(entry);
      archive_write_close(a);
      archive_write_free(a);
      throw std::runtime_error(msg);
    }

    if (is_regular) {
      std::ifstream in{ dir_entry.path(), std::ios::binary };
      if (!in) {
        archive_entry_free(entry);
        archive_write_close(a);
        archive_write_free(a);
        throw std::runtime_error("Failed to open file: " + dir_entry.path().string());
      }

      while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        auto const bytes_read{ in.gcount() };
        if (bytes_read > 0) {
          if (archive_write_data(a, buffer.data(), static_cast<size_t>(bytes_read)) < 0) {
            std::string msg{ std::string("Failed to write data: ") +
                             archive_error_string(a) };
            archive_entry_free(entry);
            archive_write_close(a);
            archive_write_free(a);
            throw std::runtime_error(msg);
          }
          bytes_processed += static_cast<std::uint64_t>(bytes_read);
          if (progress) {
            progress({ .bytes_processed = bytes_processed,
                       .files_processed = files_archived,
                       .current_entry = rel,
                       .is_regular_file = true });
          }
        }
      }
      ++files_archived;
    }
  }

  archive_entry_free(entry);
  archive_write_close(a);
  archive_write_free(a);
  return files_archived;
}

std::uint64_t extract(std::filesystem::path const &archive_path,
                      std::filesystem::path const &destination_in,
                      extract_options const &options) {
  // Resolve pre-existing symlinks in the destination prefix (e.g. macOS /var ->
  // /private/var) so ARCHIVE_EXTRACT_SECURE_SYMLINKS only trips on symlinks the
  // archive itself materializes.
  std::filesystem::path const destination{ std::filesystem::weakly_canonical(
      destination_in) };
  auto const bare_name{ extract_bare_compressed_output_name(archive_path) };
  if (bare_name && options.strip_components > 0) {
    throw std::runtime_error(
        std::string("extract: strip_components is not valid for single-stream "
                    "compressed file: ") +
        archive_path.string());
  }

  auto const selectors{ extract_normalize_selectors(options.selectors, "extract") };

  archive_reader reader{ bare_name.has_value() };
  archive_writer writer;

  if (archive_read_open_filename(reader.handle, archive_path.string().c_str(), 10240) !=
      ARCHIVE_OK) {
    throw std::runtime_error(std::string("Failed to open archive: ") +
                             archive_error_string(reader.handle));
  }

  archive_entry *entry{ nullptr };
  std::uint64_t processed{ 0 };
  std::uint64_t files_extracted{ 0 };
  std::vector<bool> selector_matched(selectors.size(), false);

  while (true) {
    int const r{ archive_read_next_header(reader.handle, &entry) };
    if (r == ARCHIVE_EOF) { break; }

    if (r != ARCHIVE_OK) {
      throw std::runtime_error(std::string("Failed to read archive header: ") +
                               archive_error_string(reader.handle));
    }

    bool const is_raw_stream{ archive_format(reader.handle) == ARCHIVE_FORMAT_RAW };

    // Raw entries carry an unhelpful pathname ("data"); substitute the derived name
    // (e.g., bar.txt.gz -> bar.txt) and treat the entry as a regular file.
    if (is_raw_stream && bare_name) {
      // Suffix promised compression, but no decompression filter matched — the file
      // is corrupt or has the wrong extension. Don't silently emit raw bytes.
      if (archive_filter_code(reader.handle, 0) == ARCHIVE_FILTER_NONE) {
        throw std::runtime_error(std::string("extract: not a valid compressed stream: ") +
                                 archive_path.string());
      }
      std::string const raw_pathname{ bare_name->string() };
      archive_entry_copy_pathname(entry, raw_pathname.c_str());
      archive_entry_set_filetype(entry, AE_IFREG);
    }

    char const *entry_path{ archive_entry_pathname(entry) };
    if (!entry_path) { throw std::runtime_error("Archive entry has null pathname"); }

    bool const is_regular_file{ archive_entry_filetype(entry) == AE_IFREG };

    std::string stripped_path;
    if (options.strip_components > 0) {
      auto stripped{ strip_path_components(entry_path, options.strip_components) };
      if (!stripped) { continue; }
      stripped_path = *stripped;
      entry_path = stripped_path.c_str();
    }

    if (!extract_is_safe_archive_path(entry_path)) {
      throw std::runtime_error(std::string("extract: unsafe archive entry path: ") +
                               entry_path);
    }

    // Kept past the pathname rewrite below so hardlink diagnostics can name the entry.
    std::string const canonical_entry{ selectors.empty()
                                           ? std::string{}
                                           : extract_canonical_match_path(entry_path) };
    if (!selectors.empty() &&
        !extract_selectors_match(selectors, canonical_entry, selector_matched)) {
      continue;  // Not selected: leave it compressed, never touch the disk.
    }

    std::filesystem::path const full_path{ destination / entry_path };
    ensure_directory(full_path);

    {
      std::string const full_path_str{ full_path.string() };
      archive_entry_copy_pathname(entry, full_path_str.c_str());
    }

    if (char const *hardlink{ archive_entry_hardlink(entry) }) {
      std::string hardlink_str{ hardlink };
      if (options.strip_components > 0) {
        auto stripped{ strip_path_components(hardlink, options.strip_components) };
        if (stripped) { hardlink_str = *stripped; }
      }
      if (!extract_is_safe_archive_path(hardlink_str.c_str())) {
        throw std::runtime_error(std::string("extract: unsafe hardlink target: ") +
                                 hardlink_str);
      }
      std::filesystem::path const hardlink_path{ destination / hardlink_str };
      // A hard link needs its target on disk. Blame the selectors only when they really
      // do exclude the target; a selected-but-absent target is an archive ordering
      // problem, and conflating the two would report the wrong cause. Checking
      // selectors here must not flag them as matched - the target entry does that.
      if (!selectors.empty()) {
        std::string const target{ extract_canonical_match_path(hardlink_str) };
        if (std::ranges::none_of(selectors, [&](std::string const &selector) {
              return extract_glob_match(selector, target);
            })) {
          throw std::runtime_error("extract: \"" + canonical_entry +
                                   "\" is a hard link to \"" + target +
                                   "\", which 'only' does not select; name it too");
        }
        if (!std::filesystem::exists(hardlink_path)) {
          throw std::runtime_error("extract: hard link target \"" + target + "\" for \"" +
                                   canonical_entry +
                                   "\" is selected but appears later in the archive");
        }
      }
      std::string const hardlink_full{ hardlink_path.string() };
      archive_entry_copy_hardlink(entry, hardlink_full.c_str());
    }

    if (options.progress &&
        !options.progress(extract_progress{ .bytes_processed = processed,
                                            .total_bytes = std::nullopt,
                                            .files_processed = 0,
                                            .total_files = std::nullopt,
                                            .current_entry = full_path,
                                            .is_regular_file = is_regular_file })) {
      throw std::runtime_error("extract: aborted by progress callback");
    }

    if (int const write_header_result{ archive_write_header(writer.handle, entry) };
        write_header_result != ARCHIVE_OK && write_header_result != ARCHIVE_WARN) {
      throw std::runtime_error(std::string("Failed to write entry header: ") +
                               archive_error_string(writer.handle));
    }

    // Raw-format entries report size as -1 (unknown); read regardless.
    if (archive_entry_size(entry) > 0 || is_raw_stream) {
      std::vector<char> buffer(1024 * 1024);

      la_ssize_t bytes_read{ 0 };
      while ((bytes_read =
                  archive_read_data(reader.handle, buffer.data(), buffer.size())) > 0) {
        if (la_ssize_t const bytes_written{
                archive_write_data(writer.handle,
                                   buffer.data(),
                                   static_cast<size_t>(bytes_read)) };
            bytes_written < 0) {
          throw std::runtime_error(std::string("Failed to write entry data: ") +
                                   archive_error_string(writer.handle));
        }

        processed += static_cast<std::uint64_t>(bytes_read);

        if (options.progress &&
            !options.progress(extract_progress{ .bytes_processed = processed,
                                                .total_bytes = std::nullopt,
                                                .files_processed = 0,
                                                .total_files = std::nullopt,
                                                .current_entry = full_path,
                                                .is_regular_file = is_regular_file })) {
          throw std::runtime_error("extract: aborted by progress callback");
        }
      }

      if (bytes_read < 0) {
        throw std::runtime_error(std::string("Failed to read entry data: ") +
                                 archive_error_string(reader.handle));
      }
    }

    if (archive_write_finish_entry(writer.handle) != ARCHIVE_OK) {
      throw std::runtime_error(std::string("Failed to finish entry: ") +
                               archive_error_string(writer.handle));
    }

    if (is_regular_file) { ++files_extracted; }
  }

  if (options.require_all_selectors) {
    throw_on_unmatched_selectors(extract_unmatched_selectors(selectors, selector_matched),
                                 "extract " + archive_path.filename().string());
  }

  // A selector list legitimately yields zero files (directories only, or nothing this
  // archive contributes); only a wholesale extraction of nothing is a failure.
  if (files_extracted == 0 && selectors.empty()) {
    std::string msg{ "Archive extraction failed: 0 files extracted from " +
                     archive_path.filename().string() };
    if (options.strip_components > 0) {
      msg += " with strip=" + std::to_string(options.strip_components) +
             ". Check if strip value matches archive structure";
    }
    msg += " (archive may be empty, corrupt, or unsupported format)";
    throw std::runtime_error(msg);
  }

  return files_extracted;
}

bool extract_is_archive_extension(std::filesystem::path const &path) {
  static std::unordered_set<std::string> const archive_extensions{
    ".tar", ".tgz", ".tar.gz", ".tar.xz", ".tar.bz2", ".tar.zst", ".zip", ".7z",
    ".rar", ".iso", ".gz",     ".bz2",    ".xz",      ".zst",     ".lzma"
  };

  std::string const ext{ path.extension().string() };
  if (archive_extensions.contains(ext)) { return true; }

  return path.stem().has_extension() &&
         archive_extensions.contains(path.stem().extension().string() + ext);
}

std::optional<std::filesystem::path> extract_bare_compressed_output_name(
    std::filesystem::path const &archive_path) {
  static std::unordered_set<std::string> const bare_extensions{ ".gz",
                                                                ".bz2",
                                                                ".xz",
                                                                ".zst",
                                                                ".lzma" };

  if (std::string const ext{ archive_path.extension().string() };
      !bare_extensions.contains(ext)) {
    return std::nullopt;
  }

  // Reject tar wrappers: foo.tar.gz, foo.tar.xz, etc. — those are archives, not bare
  // single-stream compressed payloads.
  std::filesystem::path const stem{ archive_path.filename().stem() };
  if (stem.extension() == ".tar") { return std::nullopt; }

  return stem;
}

extract_totals compute_archive_totals(std::filesystem::path const &archive_path,
                                      extract_options const &options) {
  auto const selectors{ extract_normalize_selectors(options.selectors,
                                                    "compute_archive_totals") };
  std::vector<bool> selector_matched(selectors.size(), false);
  extract_totals totals{};
  accumulate_archive_totals(archive_path,
                            selectors,
                            options.strip_components,
                            "compute_archive_totals",
                            totals,
                            selector_matched);
  totals.unmatched_selectors = extract_unmatched_selectors(selectors, selector_matched);
  return totals;
}

extract_totals compute_extract_totals(std::filesystem::path const &fetch_dir,
                                      extract_options const &options) {
  auto const selectors{ extract_normalize_selectors(options.selectors,
                                                    "compute_extract_totals") };
  std::vector<bool> selector_matched(selectors.size(), false);
  extract_totals totals{};
  if (!std::filesystem::exists(fetch_dir)) { return totals; }

  for (auto const &entry : std::filesystem::directory_iterator(fetch_dir)) {
    if (!entry.is_regular_file()) { continue; }
    if (entry.path().filename() == "envy-complete") { continue; }

    if (!extract_is_archive_extension(entry.path())) {
      // Loose files are copied verbatim, so selectors match their filename.
      if (!selectors.empty() &&
          !extract_selectors_match(selectors,
                                   entry.path().filename().generic_string(),
                                   selector_matched)) {
        continue;
      }
      std::error_code ec;
      totals.bytes += std::filesystem::file_size(entry.path(), ec);
      if (ec) {
        throw std::runtime_error("compute_extract_totals: failed to stat " +
                                 entry.path().string() + ": " + ec.message());
      }
      ++totals.files;
      continue;
    }

    accumulate_archive_totals(entry.path(),
                              selectors,
                              options.strip_components,
                              "compute_extract_totals",
                              totals,
                              selector_matched);
  }

  totals.unmatched_selectors = extract_unmatched_selectors(selectors, selector_matched);
  return totals;
}

void extract_all_archives(std::filesystem::path const &fetch_dir,
                          std::filesystem::path const &dest_dir,
                          extract_options const &options,
                          std::string const &pkg_identity,
                          tui::section_handle section) {
  if (!std::filesystem::exists(fetch_dir)) { return; }

  // Collect items to extract
  std::vector<std::string> const items{ collect_extract_items(fetch_dir) };
  if (items.empty()) { return; }

  int const strip_components{ options.strip_components };
  auto const selectors{ extract_normalize_selectors(options.selectors, "extract") };

  // Compute totals (with spinner if TUI enabled)
  std::optional<extract_tui_state> tui_state;
  if (section != tui::kInvalidSection) {
    std::string const label{ "[" + pkg_identity + "]" };
    tui::section_set_content(
        section,
        tui::section_frame{ .label = label,
                            .content = tui::spinner_data{
                                .text = "analyzing archive...",
                                .start_time = std::chrono::steady_clock::now() } });
  }

  // The pre-scan spans every archive — it, not the per-archive extract below, is where
  // a selector that nothing in the fetch dir provides gets caught.
  extract_totals const totals{ compute_extract_totals(
      fetch_dir,
      { .strip_components = strip_components, .selectors = selectors }) };
  throw_on_unmatched_selectors(totals.unmatched_selectors, "extract");

  // Set up TUI state for extraction progress
  if (section != tui::kInvalidSection) {
    tui_state.emplace(section, pkg_identity, items, totals);
    tui_state->update_progress();
  }

  std::uint64_t total_files_extracted{ 0 };
  std::uint64_t total_files_copied{ 0 };
  std::uint64_t processed_bytes{ 0 };
  // Scratch for the loose-file check below; the pre-scan owns selector validation.
  std::vector<bool> loose_selector_matched;

  for (auto const &entry : std::filesystem::directory_iterator(fetch_dir)) {
    if (!entry.is_regular_file()) { continue; }

    auto const &path{ entry.path() };
    std::string const filename{ path.filename().string() };

    if (filename == "envy-complete") { continue; }

    if (tui_state && items.size() > 1) { tui_state->on_file_start(filename); }

    if (extract_is_archive_extension(path)) {
      auto const start{ std::chrono::steady_clock::now() };

      ENVY_TRACE(extract_start,
                 pkg_identity,
                 .archive = path.string(),
                 .destination = dest_dir.string(),
                 .strip_components = strip_components);

      auto const archive_base{ processed_bytes };
      std::uint64_t last_archive_bytes{ 0 };

      extract_options opts{ .strip_components = strip_components,
                            .selectors = selectors,
                            // The pre-scan already validated the selectors against
                            // every archive; one archive alone need not satisfy it.
                            .require_all_selectors = false,
                            .progress = [&](extract_progress const &p) -> bool {
                              last_archive_bytes = p.bytes_processed;
                              if (tui_state) {
                                return tui_state->on_progress(
                                    archive_base + p.bytes_processed,
                                    p.current_entry,
                                    p.is_regular_file);
                              }
                              return true;
                            } };

      std::uint64_t const files{ extract(path, dest_dir, opts) };
      total_files_extracted += files;
      processed_bytes = archive_base + last_archive_bytes;

      auto const duration{ std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count() };

      ENVY_TRACE(extract_complete,
                 pkg_identity,
                 .archive = path.string(),
                 .files_extracted = static_cast<std::int64_t>(files),
                 .duration_ms = duration);

      if (tui_state) { tui_state->on_progress(processed_bytes, {}, false); }
    } else {
      // Loose files are copied whole, so selectors match their filename.
      if (!selectors.empty() &&
          !extract_selectors_match(selectors, filename, loose_selector_matched)) {
        continue;
      }

      std::filesystem::path const dest_path{ dest_dir / filename };
      std::filesystem::copy_file(path,
                                 dest_path,
                                 std::filesystem::copy_options::overwrite_existing);

      std::error_code ec;
      processed_bytes += std::filesystem::file_size(path, ec);
      if (ec) {
        throw std::runtime_error("extract_all_archives: failed to stat " + path.string() +
                                 ": " + ec.message());
      }

      ++total_files_copied;
      if (tui_state) { tui_state->on_progress(processed_bytes, dest_path, true); }
    }
  }

  if (tui_state) { tui_state->finish(); }

  tui::debug("stage: extracted %llu file(s) from archives, copied %llu",
             static_cast<unsigned long long>(total_files_extracted),
             static_cast<unsigned long long>(total_files_copied));
}

}  // namespace envy
