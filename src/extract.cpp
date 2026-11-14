#include "extract.h"

#include "glob.h"
#include "trace.h"
#include "tui.h"
#include "tui_actions.h"
#include "util.h"

#include "archive.h"
#include "archive_entry.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace envy {

namespace {

// What a rejected selector is called in an error; extract spells its list "only".
constexpr char kOnlyEntry[]{ "'only' entry" };
constexpr char kArchivesEntry[]{ "'archives' entry" };
constexpr char kOnlyUnmatched[]{ ": 'only' entries matched no archive contents:" };

struct archive_reader : unmovable {
  explicit archive_reader(bool enable_raw_format = false) : handle(archive_read_new()) {
    if (!handle) { throw_runtime_error({ "archive_read_new failed" }); }
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
    if (!handle) { throw_runtime_error({ "archive_write_disk_new failed" }); }
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
    throw_runtime_error({ "Failed to create directory ", dir, ": ", ec.message() });
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

// Trailing, not leading: tools that sniff a file's first bytes for zstd still see it.
constexpr std::uint32_t kTrailerMagic{ 0x184D2A55 };  // zstd's skippable range: 0x184D2A5?

// The totals an archive's trailer states, if it ends in one.
std::optional<extract_totals> trailer_read(std::filesystem::path const &archive_path) {
  extract_trailer bytes{};
  if (std::ifstream in{ archive_path, std::ios::binary };
      !in.seekg(-static_cast<std::streamoff>(bytes.size()), std::ios::end) ||
      !in.read(reinterpret_cast<char *>(bytes.data()), bytes.size())) {
    return std::nullopt;
  }
  return extract_trailer_decode(bytes);
}

// Throws msg, each unmatched selector quoted, then note; no-op when none went unmatched.
void throw_on_unmatched(std::vector<std::string> const &unmatched,
                        std::string msg,
                        std::string_view note = {}) {
  if (unmatched.empty()) { return; }
  for (auto const &u : unmatched) { msg += " \"" + u + "\""; }
  throw_runtime_error({ msg.append(note) });
}

struct extract_item {
  std::filesystem::path path;
  bool archive;  // by extension or an 'archives' include, unless an 'archives' exclude
  std::uint64_t size;
};

// fetch_dir's regular files bar the completion marker. Throws on an 'archives' include
// that names none of them.
std::vector<extract_item> collect_extract_items(std::filesystem::path const &fetch_dir,
                                                std::vector<std::string> const &archives) {
  auto const filter{ glob_parse_filter(archives, "extract", kArchivesEntry) };
  glob_match_flags matched(filter.include.size(), 0);
  std::vector<extract_item> items;
  for (auto const &entry : std::filesystem::directory_iterator(fetch_dir)) {
    if (!entry.is_regular_file() || entry.path().filename() == "envy-complete") {
      continue;
    }
    std::string const name{ entry.path().filename().generic_string() };
    bool const named{ glob_selectors_match(filter.include, name, matched) };
    items.push_back({ .path = entry.path(),
                      .archive = (named || extract_is_archive_extension(entry.path())) &&
                                 !glob_any_match(filter.exclude, name),
                      .size = entry.file_size() });
  }
  throw_on_unmatched(glob_unmatched_selectors(filter.include, matched),
                     "extract: 'archives' entries matched no fetched file:");
  return items;
}

// For an unmatched-'only' error: the loose files libarchive reads anyway, whose contents
// no selector could reach. Opens each loose file, so keep it off the success path.
std::string misnamed_archives_note(std::vector<extract_item> const &items) {
  std::string names;
  for (auto const &item : items) {
    if (item.archive || extract_is_archive_extension(item.path)) { continue; }
    archive_reader reader;
    archive_entry *entry{ nullptr };
    if (archive_read_open_filename(reader.handle, item.path.string().c_str(), 10240) ==
            ARCHIVE_OK &&
        archive_read_next_header(reader.handle, &entry) == ARCHIVE_OK) {
      names += " \"" + item.path.filename().string() + "\"";
    }
  }
  return names.empty() ? names
                       : "; these read as archives but were copied whole, lacking an "
                         "archive extension (name them in 'archives' to unpack):" +
                             names;
}

// A set states its contents only if every item does: a selected loose file is its own size,
// an archive whatever extract_probe_totals finds. nullopt at the first that cannot say.
std::optional<extract_totals> stated_totals(std::vector<extract_item> const &items,
                                            extract_options const &options,
                                            glob_filter const &selectors) {
  extract_totals sum{};
  for (auto const &item : items) {
    auto const totals{
      item.archive ? extract_probe_totals(item.path, options)
      : selectors.empty() || selectors.selects(item.path.filename().generic_string())
          ? std::optional{ extract_totals{ .files = 1, .bytes = item.size } }
          : std::optional{ extract_totals{} }
    };
    if (!totals) { return std::nullopt; }
    sum.files += totals->files;
    sum.bytes += totals->bytes;
  }
  return sum;
}

}  // namespace

std::uint64_t archive_create_tar_zst(std::filesystem::path const &output_path,
                                     std::filesystem::path const &source_dir,
                                     std::string const &prefix,
                                     extract_progress_cb_t const &progress) {
  // One walk: its totals ride on every progress call and land in the trailer.
  struct planned {
    std::filesystem::directory_entry entry;
    unsigned type;  // AE_IFLNK, AE_IFDIR or AE_IFREG; a symlink is never followed
    std::uint64_t size;
  };
  std::vector<planned> plan;
  extract_totals totals{};
  for (auto const &e : std::filesystem::recursive_directory_iterator(source_dir)) {
    if (e.is_symlink()) {
      plan.push_back({ e, AE_IFLNK, 0 });
    } else if (e.is_directory()) {
      plan.push_back({ e, AE_IFDIR, 0 });
    } else if (e.is_regular_file()) {
      plan.push_back({ e, AE_IFREG, e.file_size() });
      ++totals.files;
      totals.bytes += plan.back().size;
    }
  }

  archive *a{ archive_write_new() };
  if (!a) { throw_runtime_error({ "archive_write_new failed" }); }

  archive_write_set_format_pax_restricted(a);
  archive_write_add_filter_zstd(a);

  ensure_directory(output_path);

  if (archive_write_open_filename(a, output_path.string().c_str()) != ARCHIVE_OK) {
    std::string msg{ std::string("Failed to open output: ") + archive_error_string(a) };
    archive_write_free(a);
    throw_runtime_error({ msg });
  }

  archive_entry *entry{ archive_entry_new() };
  std::uint64_t files_archived{ 0 };
  std::uint64_t bytes_processed{ 0 };
  std::vector<char> buffer(1024 * 1024);

  auto const report{ [&](std::filesystem::path const &rel, bool is_regular) {
    if (progress) {
      progress({ .bytes_processed = bytes_processed,
                 .files_processed = files_archived,
                 .totals = totals,
                 .current_entry = rel,
                 .is_regular_file = is_regular });
    }
  } };

  for (auto const &[dir_entry, type, size] : plan) {
    std::filesystem::path const rel{ dir_entry.path().lexically_relative(source_dir) };
    std::string const archived_path{ prefix + "/" + rel.generic_string() };
    bool const is_regular{ type == AE_IFREG };

    archive_entry_clear(entry);
    archive_entry_set_pathname(entry, archived_path.c_str());
    archive_entry_set_filetype(entry, type);
    archive_entry_set_size(entry, static_cast<la_int64_t>(size));
    if (type == AE_IFLNK) {
      archive_entry_set_symlink(
          entry,
          std::filesystem::read_symlink(dir_entry.path()).string().c_str());
    }

    // Preserve permissions
    std::error_code ec;
    auto const perms{ std::filesystem::status(dir_entry.path(), ec).permissions() };
    if (!ec) { archive_entry_set_perm(entry, static_cast<__LA_MODE_T>(perms)); }

    if (is_regular) { ++files_archived; }  // begun, as extract() counts
    report(rel, is_regular);

    if (archive_write_header(a, entry) != ARCHIVE_OK) {
      std::string msg{ std::string("Failed to write header: ") + archive_error_string(a) };
      archive_entry_free(entry);
      archive_write_close(a);
      archive_write_free(a);
      throw_runtime_error({ msg });
    }

    if (is_regular) {
      std::ifstream in{ dir_entry.path(), std::ios::binary };
      if (!in) {
        archive_entry_free(entry);
        archive_write_close(a);
        archive_write_free(a);
        throw_runtime_error({ "Failed to open file: ", dir_entry.path() });
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
            throw_runtime_error({ msg });
          }
          bytes_processed += static_cast<std::uint64_t>(bytes_read);
          report(rel, true);
        }
      }
    }
  }

  archive_entry_free(entry);
  archive_write_close(a);
  archive_write_free(a);

  // Header sizes, not bytes read: libarchive wrote exactly what each header promised.
  if (auto const trailer{ extract_trailer_encode(totals) };
      !std::ofstream{ output_path, std::ios::binary | std::ios::app }
           .write(reinterpret_cast<char const *>(trailer.data()), trailer.size())
           .flush()) {
    throw_runtime_error({ "Failed to write archive trailer: ", output_path });
  }
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
    throw_runtime_error(
        { "extract: strip_components is not valid for single-stream "
          "compressed file: ",
          archive_path });
  }

  auto const selectors{ glob_parse_filter(options.selectors, "extract", kOnlyEntry) };

  archive_reader reader{ bare_name.has_value() };
  archive_writer writer;

  if (archive_read_open_filename(reader.handle, archive_path.string().c_str(), 10240) !=
      ARCHIVE_OK) {
    throw_runtime_error(
        { "Failed to open archive: ", archive_error_string(reader.handle) });
  }

  archive_entry *entry{ nullptr };
  std::uint64_t processed{ 0 };
  std::uint64_t files_extracted{ 0 };
  glob_match_flags own_matched;
  glob_match_flags &selector_matched{ options.selector_matched ? *options.selector_matched
                                                                : own_matched };

  // The header and every chunk after it report alike; the archive's read position is the
  // only progress measure an archive that states no totals has.
  auto const report{ [&](std::filesystem::path const &current, bool is_regular_file) {
    if (la_int64_t const read{ archive_filter_bytes(reader.handle, -1) };
        options.progress &&
        !options.progress({ .bytes_processed = processed,
                            .files_processed = files_extracted,
                            .input_bytes_processed =
                                read > 0 ? static_cast<std::uint64_t>(read) : 0,
                            .current_entry = current,
                            .is_regular_file = is_regular_file })) {
      throw_runtime_error({ "extract: aborted by progress callback" });
    }
  } };

  while (true) {
    int const r{ archive_read_next_header(reader.handle, &entry) };
    if (r == ARCHIVE_EOF) { break; }

    if (r != ARCHIVE_OK) {
      throw_runtime_error(
          { "Failed to read archive header: ", archive_error_string(reader.handle) });
    }

    bool const is_raw_stream{ archive_format(reader.handle) == ARCHIVE_FORMAT_RAW };

    // Raw entries carry an unhelpful pathname ("data"); substitute the derived name
    // (e.g., bar.txt.gz -> bar.txt) and treat the entry as a regular file.
    if (is_raw_stream && bare_name) {
      // Suffix promised compression, but no decompression filter matched — the file
      // is corrupt or has the wrong extension. Don't silently emit raw bytes.
      if (archive_filter_code(reader.handle, 0) == ARCHIVE_FILTER_NONE) {
        throw_runtime_error({ "extract: not a valid compressed stream: ", archive_path });
      }
      std::string const raw_pathname{ bare_name->string() };
      archive_entry_copy_pathname(entry, raw_pathname.c_str());
      archive_entry_set_filetype(entry, AE_IFREG);
    }

    char const *entry_path{ archive_entry_pathname(entry) };
    if (!entry_path) { throw_runtime_error({ "Archive entry has null pathname" }); }

    bool const is_regular_file{ archive_entry_filetype(entry) == AE_IFREG };

    std::string stripped_path;
    if (options.strip_components > 0) {
      auto stripped{ strip_path_components(entry_path, options.strip_components) };
      if (!stripped) { continue; }
      stripped_path = *stripped;
      entry_path = stripped_path.c_str();
    }

    if (!glob_is_safe_relative_path(entry_path)) {
      throw_runtime_error({ "extract: unsafe archive entry path: ", entry_path });
    }

    // Kept past the pathname rewrite below so hardlink diagnostics can name the entry.
    std::string const canonical_entry{ selectors.empty()
                                           ? std::string{}
                                           : glob_canonical_path(entry_path) };
    if (!selectors.empty() && !selectors.selects(canonical_entry, selector_matched)) {
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
      if (!glob_is_safe_relative_path(hardlink_str.c_str())) {
        throw_runtime_error({ "extract: unsafe hardlink target: ", hardlink_str });
      }
      std::filesystem::path const hardlink_path{ destination / hardlink_str };
      // A hard link needs its target on disk. Blame the selectors only when they really
      // do exclude the target; a selected-but-absent target is an archive ordering
      // problem, and conflating the two would report the wrong cause. Checking
      // selectors here must not flag them as matched - the target entry does that.
      if (!selectors.empty()) {
        std::string const target{ glob_canonical_path(hardlink_str) };
        // The whole filter, not just the includes: an exclude-only list selects the
        // target, and an exclude that names it means the filter really does reject it.
        if (!selectors.selects(target)) {
          throw_runtime_error({ "extract: \"",
                                canonical_entry,
                                "\" is a hard link to \"",
                                target,
                                "\", which 'only' does not select; name it too" });
        }
        if (!std::filesystem::exists(hardlink_path)) {
          throw_runtime_error({ "extract: hard link target \"",
                                target,
                                "\" for \"",
                                canonical_entry,
                                "\" is selected but appears later in the archive" });
        }
      }
      std::string const hardlink_full{ hardlink_path.string() };
      archive_entry_copy_hardlink(entry, hardlink_full.c_str());
    }

    if (is_regular_file) { ++files_extracted; }
    report(full_path, is_regular_file);

    if (int const write_header_result{ archive_write_header(writer.handle, entry) };
        write_header_result != ARCHIVE_OK && write_header_result != ARCHIVE_WARN) {
      throw_runtime_error(
          { "Failed to write entry header: ", archive_error_string(writer.handle) });
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
          throw_runtime_error(
              { "Failed to write entry data: ", archive_error_string(writer.handle) });
        }

        processed += static_cast<std::uint64_t>(bytes_read);
        report(full_path, is_regular_file);
      }

      if (bytes_read < 0) {
        throw_runtime_error(
            { "Failed to read entry data: ", archive_error_string(reader.handle) });
      }
    }

    if (archive_write_finish_entry(writer.handle) != ARCHIVE_OK) {
      throw_runtime_error(
          { "Failed to finish entry: ", archive_error_string(writer.handle) });
    }
  }

  if (!options.selector_matched) {
    throw_on_unmatched(glob_unmatched_selectors(selectors.include, selector_matched),
                       "extract " + archive_path.filename().string() + kOnlyUnmatched);
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
    throw_runtime_error({ msg });
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

extract_trailer extract_trailer_encode(extract_totals const &totals) {
  extract_trailer b{};
  auto const put{ [&](std::size_t at, std::uint64_t v, std::size_t n) {
    for (std::size_t i{ 0 }; i < n; ++i) { b[at + i] = static_cast<std::uint8_t>(v >> 8 * i); }
  } };
  put(0, kTrailerMagic, 4);
  put(4, b.size() - 8, 4);  // payload: files, bytes, tag
  put(8, totals.files, 8);
  put(16, totals.bytes, 8);
  std::memcpy(b.data() + 24, "envytot1", 8);
  return b;
}

std::optional<extract_totals> extract_trailer_decode(extract_trailer const &bytes) {
  auto const get{ [&](std::size_t at) {
    std::uint64_t v{ 0 };
    for (std::size_t i{ 8 }; i-- > 0;) { v = v << 8 | bytes[at + i]; }
    return v;
  } };
  extract_totals const totals{ .files = get(8), .bytes = get(16) };
  return extract_trailer_encode(totals) == bytes ? std::optional{ totals } : std::nullopt;
}

std::optional<extract_totals> extract_probe_totals(std::filesystem::path const &archive_path,
                                                   extract_options const &options) {
  if (options.selectors.empty() && options.strip_components == 0) {  // whole archive only
    if (auto const stated{ trailer_read(archive_path) }) { return stated; }
  }

  auto const selectors{ glob_parse_filter(options.selectors, "extract", kOnlyEntry) };
  archive_reader reader;
  if (archive_read_open_filename(reader.handle, archive_path.string().c_str(), 10240) !=
      ARCHIVE_OK) {
    return std::nullopt;
  }

  extract_totals totals{};
  archive_entry *entry{ nullptr };
  for (bool first{ true };; first = false) {
    if (int const r{ archive_read_next_header(reader.handle, &entry) }; r != ARCHIVE_OK) {
      // EOF ends the count; an error is extract()'s to report.
      return r == ARCHIVE_EOF ? std::optional{ totals } : std::nullopt;
    }

    // Skipping an entry is a seek only with no decompression filter, in a format whose
    // entries lie whole in the file; anywhere else, counting decompresses everything.
    if (int const format{ archive_format(reader.handle) & ARCHIVE_FORMAT_BASE_MASK };
        first && (archive_filter_code(reader.handle, 0) != ARCHIVE_FILTER_NONE ||
                  (format != ARCHIVE_FORMAT_TAR && format != ARCHIVE_FORMAT_ZIP))) {
      return std::nullopt;
    }

    // Exactly what extract() would write: strip, then 'only', then regular files.
    if (auto const stripped{
          strip_path_components(archive_entry_pathname(entry), options.strip_components) };
        !stripped ||
        (!selectors.empty() && !selectors.selects(glob_canonical_path(*stripped))) ||
        archive_entry_filetype(entry) != AE_IFREG) {
      continue;
    }
    la_int64_t const size{ archive_entry_size(entry) };
    ++totals.files;
    totals.bytes += size > 0 ? static_cast<std::uint64_t>(size) : 0;
  }
}

void extract_all_archives(std::filesystem::path const &fetch_dir,
                          std::filesystem::path const &dest_dir,
                          extract_options const &options,
                          std::string const &pkg_identity,
                          tui::section_handle section) {
  if (!std::filesystem::exists(fetch_dir)) { return; }

  std::vector<extract_item> const items{ collect_extract_items(fetch_dir,
                                                               options.archives) };
  if (items.empty()) { return; }

  // 'only' holds for the set, not each item: every archive and loose file flags the same
  // includes. Loose files are copied whole, so selectors match their filename.
  auto const selectors{ glob_parse_filter(options.selectors, "extract", kOnlyEntry) };
  glob_match_flags selector_matched(selectors.include.size(), 0);

  std::optional<tui_actions::extract_progress_tracker> tracker;
  if (section != tui::kInvalidSection) {
    std::vector<std::string> names;
    std::uint64_t input_total{ 0 };
    for (auto const &item : items) {
      names.push_back(item.path.filename().string());
      input_total += item.size;
    }
    tracker.emplace(section,
                    pkg_identity,
                    std::move(names),
                    input_total,
                    stated_totals(items, options, selectors));
  }

  std::uint64_t total_files_extracted{ 0 };
  std::uint64_t total_files_copied{ 0 };

  for (std::size_t i{ 0 }; i < items.size(); ++i) {
    auto const &[path, archive, size]{ items[i] };
    std::string const filename{ path.filename().string() };
    if (tracker) { tracker->begin(i); }

    if (archive) {
      auto const start{ std::chrono::steady_clock::now() };
      ENVY_TRACE(extract_start,
                 pkg_identity,
                 .archive = path.string(),
                 .destination = dest_dir.string(),
                 .strip_components = options.strip_components);

      std::uint64_t const files{ extract(
          path,
          dest_dir,
          { .strip_components = options.strip_components,
            .selectors = options.selectors,
            .selector_matched = &selector_matched,
            .progress = tracker ? extract_progress_cb_t{ std::ref(*tracker) }
                                : extract_progress_cb_t{} }) };
      total_files_extracted += files;

      ENVY_TRACE(extract_complete,
                 pkg_identity,
                 .archive = path.string(),
                 .files_extracted = static_cast<std::int64_t>(files),
                 .duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - start)
                                    .count());
    } else if (selectors.empty() || selectors.selects(filename, selector_matched)) {
      std::filesystem::path const dest_path{ dest_dir / filename };
      std::filesystem::copy_file(path,
                                 dest_path,
                                 std::filesystem::copy_options::overwrite_existing);
      ++total_files_copied;
      if (tracker) {
        (*tracker)({ .bytes_processed = size,
                     .files_processed = 1,
                     .input_bytes_processed = size,
                     .current_entry = dest_path,
                     .is_regular_file = true });
      }
    }

    if (tracker) { tracker->end(size); }
  }

  if (auto const unmatched{ glob_unmatched_selectors(selectors.include, selector_matched) };
      !unmatched.empty()) {
    throw_on_unmatched(unmatched,
                       std::string{ "extract" } + kOnlyUnmatched,
                       misnamed_archives_note(items));
  }

  if (tracker) { tracker->finish(); }

  tui::debug("stage: extracted %llu file(s) from archives, copied %llu",
             static_cast<unsigned long long>(total_files_extracted),
             static_cast<unsigned long long>(total_files_copied));
}

}  // namespace envy
