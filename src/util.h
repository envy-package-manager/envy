#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
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

// One piece of an exception message: viewed in place where it is thrown, joined out of
// line, so a throw site builds no strings and needs no cleanup for them.
class err_part {
 public:
  err_part(char const *s) noexcept : kind_{ kind::CSTR }, cstr_{ s } {}
  err_part(std::string const &s) noexcept : kind_{ kind::STR }, str_{ &s } {}
  err_part(std::string_view s) noexcept : kind_{ kind::VIEW }, view_{ s } {}
  err_part(std::filesystem::path const &p) noexcept : kind_{ kind::PATH }, path_{ &p } {}
  template <std::integral T>
    requires(!std::is_same_v<T, bool> && !std::is_same_v<T, char>)
  err_part(T v) noexcept
      : kind_{ std::is_signed_v<T> ? kind::INT : kind::UINT },
        uint_{ static_cast<unsigned long long>(v) } {}

  void append_to(std::string &out) const;

 private:
  enum class kind : unsigned char { CSTR, STR, VIEW, PATH, INT, UINT };
  kind kind_;
  union {
    char const *cstr_;
    std::string const *str_;
    std::string_view view_;
    std::filesystem::path const *path_;
    unsigned long long uint_;  // INT reads it back as signed
  };
};

// throw std::runtime_error(a + b + ...) as throw_runtime_error({ a, b, ... }): the
// concatenation happens once, out of line, instead of inline at every throw site.
[[noreturn]] void throw_runtime_error(std::initializer_list<err_part> parts);
[[noreturn]] void throw_system_error(int ev,
                                     std::error_category const &cat,
                                     std::initializer_list<err_part> parts);

[[noreturn]] void util_throw_bad_variant_access();

// std::visit's exhaustiveness without its dispatch table: an index chain inlined into the
// caller, so a visit costs what its handlers cost. Call qualified; ADL also finds std's.
template <std::size_t I = 0, typename F, typename V>
constexpr decltype(auto) visit(F &&f, V &&v) {
  auto *const alt{ std::get_if<I>(&v) };
  if constexpr (I + 1 < std::variant_size_v<std::remove_cvref_t<V>>) {
    if (!alt) { return envy::visit<I + 1>(std::forward<F>(f), std::forward<V>(v)); }
  } else if (!alt) {
    util_throw_bad_variant_access();  // valueless_by_exception
  }
  if constexpr (std::is_lvalue_reference_v<V>) {
    return std::forward<F>(f)(*alt);
  } else {
    return std::forward<F>(f)(std::move(*alt));
  }
}

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::lifetimebound)
#define ENVY_LIFETIMEBOUND [[clang::lifetimebound]]
#endif
#endif
#ifndef ENVY_LIFETIMEBOUND
#define ENVY_LIFETIMEBOUND
#endif

// Non-owning callable reference: two words, no allocation, no RTTI. Takes a callable that
// outlives the call (a lambda argument), never one stored past it.
template <typename Sig>
class function_ref;

template <typename R, typename... Args>
class function_ref<R(Args...)> {
 public:
  function_ref() = default;

  template <typename F>
    requires(!std::is_same_v<std::remove_cvref_t<F>, function_ref> &&
             !std::is_function_v<std::remove_reference_t<F>> &&
             std::is_invocable_r_v<R, F &, Args...>)
  function_ref(F &&f ENVY_LIFETIMEBOUND) noexcept {  // NOLINT(google-explicit-constructor)
    // A null function pointer or an empty std::function stays empty. Not a captureless
    // lambda: it converts to bool only through its function pointer (MSVC C4305).
    if constexpr (std::is_pointer_v<std::remove_cvref_t<F>>) {
      if (f == nullptr) { return; }
    } else if constexpr (std::is_constructible_v<bool, F &> &&
                         !std::is_convertible_v<F &, bool>) {
      if (!static_cast<bool>(f)) { return; }
    }
    obj_ = const_cast<void *>(static_cast<void const *>(std::addressof(f)));
    call_ = [](void *obj, Args... args) -> R {
      auto &fn{ *static_cast<std::remove_reference_t<F> *>(obj) };
      if constexpr (std::is_void_v<R>) {
        fn(std::forward<Args>(args)...);  // a void signature discards any result
      } else {
        return fn(std::forward<Args>(args)...);
      }
    };
  }

  R operator()(Args... args) const { return call_(obj_, std::forward<Args>(args)...); }
  explicit operator bool() const { return call_ != nullptr; }

 private:
  void *obj_{ nullptr };
  R (*call_)(void *, Args...){ nullptr };
};

// Index order[0..n) sorted by `less`, ties by index: stable, and one std::sort for all.
void util_sort_order(std::vector<std::size_t> &order,
                     function_ref<bool(std::size_t, std::size_t)> less);

// Stable sort through util_sort_order: a call site costs a comparator thunk and a
// permutation loop, not a whole std::sort instantiation.
template <typename T, typename Less>
void util_sort(std::vector<T> &v, Less const &less) {
  std::vector<std::size_t> order(v.size());
  util_sort_order(order, [&](std::size_t a, std::size_t b) { return less(v[a], v[b]); });
  for (std::size_t i{ 0 }; i < order.size();
       ++i) {  // apply the permutation cycle by cycle
    if (order[i] == i) { continue; }
    T held{ std::move(v[i]) };
    std::size_t j{ i };
    for (std::size_t k{ order[j] }; k != i; k = order[j]) {
      v[j] = std::move(v[k]);
      order[j] = j;
      j = k;
    }
    v[j] = std::move(held);
    order[j] = j;
  }
}

template <typename T>
void util_sort(std::vector<T> &v) {
  util_sort(v, [](T const &a, T const &b) { return a < b; });
}

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

// A gzip stream plus the byte count it inflates to; instances are emitted by
// cmake/scripts/embed_resource.py --compress.
// One embedded resource: bytes [offset, offset + size) of the xz stream `pack`.
struct packed_resource {
  unsigned char const *pack;
  size_t pack_size;
  size_t offset;
  size_t size;
};

// Unpack an embedded resource. Throws std::runtime_error on a malformed stream.
std::string util_unpack_resource(packed_resource const &res);

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

// Target-keyed and idempotent: CRLF for WINDOWS (cmd.exe label seeks assume it), LF else.
void util_apply_script_eol(std::string &script, platform_id plat);

enum class script_write { UNCHANGED, CREATED, UPDATED };

script_write util_write_script(std::filesystem::path const &path,
                               std::string content,
                               platform_id plat);

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

// Absolute, symlink-resolved form of `path`; plain absolute when canonicalization fails
// (a component that does not exist yet). Anchors have to be canonical before an upward
// walk: parent_path() on a relative path bottoms out at its own first segment.
std::filesystem::path util_canonical_path(std::filesystem::path const &path);

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
