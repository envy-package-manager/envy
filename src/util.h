#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace envy {

struct uncopyable {
  uncopyable() = default;
  uncopyable(uncopyable &&) = default;
  uncopyable &operator=(uncopyable &&) = default;
};

struct unmovable {
  unmovable() = default;
  unmovable(unmovable const &) = delete;
  unmovable &operator=(unmovable const &) = delete;
};

template <typename... Ts>
struct match : Ts... {
  using Ts::operator()...;
};

template <typename... Ts>
match(Ts...) -> match<Ts...>;

// Convert bytes to lowercase hex string
std::string util_bytes_to_hex(void const *data, size_t length);

// Locale-independent ASCII character classification. std::isalpha/isalnum are
// locale-dependent and can misclassify high-bit UTF-8 bytes; use these for path
// and identity parsing so behavior is deterministic on any locale.
constexpr bool util_ascii_is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
constexpr bool util_ascii_is_alnum(char c) {
  return util_ascii_is_alpha(c) || (c >= '0' && c <= '9');
}

// True if s is safe to use as a single path component: non-empty, not "." / "..",
// and every char is ASCII alnum or one of ". - _ @". Rejects path separators, drive
// letters, and anything else that could escape a containing directory.
bool util_is_safe_path_component(std::string_view s);

// Escape a string for JSON output (RFC 8259 compliant).
// Handles \", \\, \b, \f, \n, \r, \t, and \u00xx for other control chars < 0x20.
std::string util_escape_json_string(std::string_view value);

// Convert hex string to bytes (case-insensitive)
std::vector<unsigned char> util_hex_to_bytes(std::string const &hex);

// Convert single hex character to value (0-15). Returns -1 if invalid.
int util_hex_char_to_int(char c);

// RAII file pointer with custom deleter
struct file_deleter {
  void operator()(std::FILE *file) const noexcept;
};
using file_ptr_t = std::unique_ptr<std::FILE, file_deleter>;

// Open file with RAII wrapper. Returns nullptr on failure.
// On Windows, uses _wfopen for proper Unicode path support.
file_ptr_t util_open_file(std::filesystem::path const &path, char const *mode);

// Load entire file into memory as bytes.
// Throws std::runtime_error if file cannot be opened or read.
std::vector<unsigned char> util_load_file(std::filesystem::path const &path);

// Write content to file atomically (temp file + rename).
// Parent directory must exist. Throws std::runtime_error on failure.
void util_write_file(std::filesystem::path const &path, std::string_view content);

// Human-readable byte formatter (B, KB, MB, GB, TB). B uses integer form, higher
// units use one decimal place with rounding (e.g., 1536 -> "1.5KB").
std::string util_format_bytes(std::uint64_t bytes);

// Flatten multi-line script to single line with semicolon delimiters.
// Replaces newlines (\n, \r\n, \r) with "; ", collapses consecutive spaces/tabs to single
// space. Trims trailing semicolons and whitespace. Example: "cmd1\ncmd2\ncmd3" -> "cmd1;
// cmd2; cmd3"
std::string util_flatten_script_with_semicolons(std::string_view script);

// Convert filesystem path to string with trailing separator.
// Ensures Lua expressions like `dir .. "filename"` produce correct paths.
// A path as a spec sees it, with separators uniformly the platform's own. envy
// assembles paths from a cache root, manifest text and Lua fragments, any of which
// may be spelled with either separator, so joining alone yields things like
// "C:/cache/pkg\\file". Every path handed to a spec goes through here so none does.
std::string util_normalized_path(std::filesystem::path path);

std::string util_path_with_separator(std::filesystem::path const &path);

// Forward-declared from platform.h (can't include here — platform.h includes util.h).
enum class platform_id;

std::vector<platform_id> util_parse_platform_flag(std::string const &value);

// Empty constraints = match all. Checks target_os or "target_os-target_arch" membership.
bool util_platform_matches(std::vector<std::string> const &constraints,
                           std::string_view target_os,
                           std::string_view target_arch);

// Empty = "all"; two non-empty lists yield set intersection.
std::vector<std::string> util_platform_intersect(std::vector<std::string> const &a,
                                                 std::vector<std::string> const &b);

// Sentinel platform constraint that matches nothing; used when disjoint
// non-empty platform lists intersect to an empty set (empty = "all").
inline constexpr char const *kPlatformNone = "__none__";

// Does constraint list produce scripts for this platform_id?
// POSIX covers "darwin" and "linux"; WINDOWS covers "windows".
bool util_platform_matches_platform_id(std::vector<std::string> const &constraints,
                                       platform_id plat);

// Product mapping: pairs of (product_name, relative_path)
// Example: {"cmake", "bin/cmake.exe"} or {"python", "bin/python3"}
using product_map_t = std::vector<std::pair<std::string, std::string>>;

// Resolve relative path against an anchor directory.
// Throws if `relative` is absolute, or if `anchor` is not an absolute path.
std::filesystem::path util_absolute_path(std::filesystem::path const &relative,
                                         std::filesystem::path const &anchor);

// Simplify cache paths in command string for display.
// First tries to match tokens against product paths (suffix matching).
// Falls back to cache_root prefix detection with filename extraction.
// Example with products: "/cache/.../bin/cmake.exe" -> "cmake" (if products has
// cmake->bin/cmake.exe) Example without: "/cache/assets/pkg/bin/python" -> "python" (if
// starts with cache_root)
std::string util_simplify_cache_paths(std::string_view command,
                                      std::filesystem::path const &cache_root,
                                      product_map_t const &products = {});

// Parsed fields from an exported archive filename stem.
// Stem format: <name>@<revision>-<platform>-<arch>-blake3-<hash_prefix>
// '@' is required; content before/after '@' is not strictly validated.
struct parsed_archive_filename {
  std::string identity;     // e.g. "arm.gcc@r2"
  std::string platform;     // e.g. "darwin"
  std::string arch;         // e.g. "arm64"
  std::string hash_prefix;  // e.g. "abcdef0123456789"
};

// Parse exported archive filename stem (without .tar.zst extension).
// Returns nullopt on invalid format.
std::optional<parsed_archive_filename> util_parse_archive_filename(std::string_view stem);

class scoped_path_cleanup : public unmovable {
 public:
  explicit scoped_path_cleanup(std::filesystem::path path);
  ~scoped_path_cleanup();

  void reset(std::filesystem::path path = {});
  std::filesystem::path const &path() const { return path_; }

 private:
  void cleanup();

  std::filesystem::path path_;
};

}  // namespace envy
