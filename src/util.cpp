#include "util.h"

#include "platform.h"

#include "zlib_compat.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <string>
#endif

namespace envy {

bool util_is_safe_path_component(std::string_view s) {
  if (s.empty() || s == "." || s == "..") { return false; }
  // Strict ASCII whitelist, locale-independent (see util_ascii_is_alnum): identities
  // are deliberately ASCII-only and become cache/spec path components.
  for (char const c : s) {
    if (!util_ascii_is_alnum(c) && c != '.' && c != '-' && c != '_' && c != '@') {
      return false;
    }
  }
  return true;
}

std::string util_bytes_to_hex(void const *data, size_t length) {
  static constexpr char hex_chars[] = "0123456789abcdef";

  auto const bytes = static_cast<unsigned char const *>(data);
  std::string result;
  result.reserve(length * 2);

  for (size_t i{}; i < length; ++i) {
    result += hex_chars[(bytes[i] >> 4) & 0xf];
    result += hex_chars[bytes[i] & 0xf];
  }

  return result;
}

std::string util_escape_json_string(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (char const ch : value) {
    switch (ch) {
      case '\\': out.append("\\\\"); break;
      case '"': out.append("\\\""); break;
      case '\b': out.append("\\b"); break;
      case '\f': out.append("\\f"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          static constexpr char hex[] = "0123456789abcdef";
          out.append("\\u00");
          out.push_back(hex[(ch >> 4) & 0xF]);
          out.push_back(hex[ch & 0xF]);
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  return out;
}

int util_hex_char_to_int(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
  if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
  return -1;
}

std::vector<unsigned char> util_hex_to_bytes(std::string const &hex) {
  if (hex.size() % 2 != 0) {
    throw std::runtime_error("util_hex_to_bytes: hex string must have even length, got " +
                             std::to_string(hex.size()));
  }

  std::vector<unsigned char> result;
  result.reserve(hex.size() / 2);

  for (size_t i{}; i < hex.size(); i += 2) {
    int const hi{ util_hex_char_to_int(hex[i]) };
    int const lo{ util_hex_char_to_int(hex[i + 1]) };

    if (hi < 0) {
      throw std::runtime_error(
          std::string("util_hex_to_bytes: invalid character at position ") +
          std::to_string(i));
    }
    if (lo < 0) {
      throw std::runtime_error(
          std::string("util_hex_to_bytes: invalid character at position ") +
          std::to_string(i + 1));
    }

    result.push_back(static_cast<unsigned char>((hi << 4) | lo));
  }

  return result;
}

void file_deleter::operator()(std::FILE *file) const noexcept {
  if (file) { static_cast<void>(std::fclose(file)); }
}

file_ptr_t util_open_file(std::filesystem::path const &path, char const *mode) {
#if defined(_WIN32)
  // Convert mode string to wide string for _wfopen
  std::wstring wide_mode;
  wide_mode.reserve(std::strlen(mode));
  for (char const *p{ mode }; *p != '\0'; ++p) {
    wide_mode.push_back(static_cast<wchar_t>(*p));
  }
  return file_ptr_t{ _wfopen(path.c_str(), wide_mode.c_str()) };
#else
  return file_ptr_t{ std::fopen(path.c_str(), mode) };
#endif
}

std::vector<unsigned char> util_load_file(std::filesystem::path const &path) {
  auto file{ util_open_file(path, "rb") };
  if (!file) {
    throw std::runtime_error("util_load_file: failed to open file: " + path.string());
  }

  // Get file size
  if (std::fseek(file.get(), 0, SEEK_END) != 0) {
    throw std::runtime_error("util_load_file: failed to seek to end: " + path.string());
  }

  long const file_size{ std::ftell(file.get()) };
  if (file_size < 0) {
    throw std::runtime_error("util_load_file: failed to get file size: " + path.string());
  }

  if (std::fseek(file.get(), 0, SEEK_SET) != 0) {
    throw std::runtime_error("util_load_file: failed to seek to start: " + path.string());
  }

  // Read entire file
  std::vector<unsigned char> buffer(static_cast<size_t>(file_size));
  if (file_size > 0) {
    size_t const bytes_read{ std::fread(buffer.data(), 1, buffer.size(), file.get()) };
    if (bytes_read != buffer.size()) {
      throw std::runtime_error("util_load_file: failed to read entire file: " +
                               path.string());
    }
  }

  return buffer;
}

void util_write_file(std::filesystem::path const &path, std::string_view content) {
  namespace fs = std::filesystem;
  fs::path const temp_path{ path.parent_path() /
                            (".envy-tmp-" + path.filename().string()) };

  {
    std::ofstream out{ temp_path, std::ios::binary };
    if (!out) {
      throw std::runtime_error("util_write_file: failed to create " + temp_path.string());
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out.good()) {
      throw std::runtime_error("util_write_file: failed to write " + temp_path.string());
    }
  }

  std::error_code ec;
  fs::rename(temp_path, path, ec);
  if (ec) {
    fs::remove(temp_path, ec);
    throw std::runtime_error("util_write_file: failed to rename " + temp_path.string() +
                             " to " + path.string() + ": " + ec.message());
  }
}

std::string util_inflate_resource(gz_resource const &res) {
  // zlib counts in 32-bit uInt. Every caller passes a build-time constant far below that,
  // so exceeding it means the generator is broken, not that the input is legitimately big.
  constexpr size_t kZlibMax{ std::numeric_limits<uInt>::max() };
  if (res.size > kZlibMax || res.inflated_size > kZlibMax) {
    throw std::runtime_error(
        "util_inflate_resource: resource exceeds zlib's 32-bit "
        "limit");
  }

  z_stream strm{};
  strm.next_in = const_cast<Bytef *>(res.data);
  strm.avail_in = static_cast<uInt>(res.size);
  if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {  // 16 + MAX_WBITS: gzip header
    throw std::runtime_error("util_inflate_resource: inflateInit2 failed");
  }

  std::string out;
  out.resize(res.inflated_size ? res.inflated_size : 64 * 1024);
  strm.next_out = reinterpret_cast<Bytef *>(out.data());
  strm.avail_out = static_cast<uInt>(out.size());

  int ret{ Z_OK };
  while (ret != Z_STREAM_END) {
    ret = inflate(&strm, Z_NO_FLUSH);
    // Z_BUF_ERROR with no room left means "grow me"; with room left it means the stream
    // ended early, which is corruption and falls through to the throw below.
    if (ret == Z_BUF_ERROR && strm.avail_out == 0) {
      size_t const grown{ out.size() };
      if (grown > kZlibMax / 2) {
        inflateEnd(&strm);
        throw std::runtime_error(
            "util_inflate_resource: inflated size exceeds zlib's "
            "32-bit limit");
      }
      out.resize(grown * 2);
      strm.next_out = reinterpret_cast<Bytef *>(out.data()) + grown;
      strm.avail_out = static_cast<uInt>(grown);
    } else if (ret != Z_OK && ret != Z_STREAM_END) {
      inflateEnd(&strm);
      throw std::runtime_error("util_inflate_resource: corrupt gzip stream");
    }
  }

  inflateEnd(&strm);
  // A declared size that disagrees with the stream means the two halves of the embedding
  // disagree; surface that rather than silently handing back a truncated resource.
  if (res.inflated_size && strm.total_out != res.inflated_size) {
    throw std::runtime_error("util_inflate_resource: inflated size mismatch");
  }
  out.resize(strm.total_out);
  return out;
}

std::string util_format_bytes(std::uint64_t bytes) {
  static constexpr std::array<char const *, 5> kUnits{ "B", "KB", "MB", "GB", "TB" };

  double value{ static_cast<double>(bytes) };
  std::size_t unit{ 0 };

  while (value >= 1024.0 && unit + 1 < kUnits.size()) {
    value /= 1024.0;
    ++unit;
  }

  if (unit == 0) { return std::to_string(static_cast<std::uint64_t>(value)) + "B"; }

  std::ostringstream oss;
  oss.setf(std::ios::fixed, std::ios::floatfield);
  oss << std::setprecision(2) << value << kUnits[unit];
  return oss.str();
}

std::string util_normalized_path(std::filesystem::path path) {
  return path.make_preferred().string();
}

std::string util_path_with_separator(std::filesystem::path const &path) {
  std::string result{ util_normalized_path(path) };
  if (result.empty()) { return result; }

  // Normalized, so the only separator that can already be trailing is the preferred
  // one; anything else is a literal character in a filename.
  char const sep{ static_cast<char>(std::filesystem::path::preferred_separator) };
  if (result.back() != sep) { result += sep; }

  return result;
}

std::filesystem::path util_absolute_path(std::filesystem::path const &relative,
                                         std::filesystem::path const &anchor) {
  if (relative.is_absolute()) {
    throw std::runtime_error("util_absolute_path: path must be relative, got: " +
                             relative.string());
  }
  if (!anchor.is_absolute()) {
    throw std::runtime_error("util_absolute_path: anchor must be absolute, got: " +
                             anchor.string());
  }
  return (anchor / relative).lexically_normal();
}

std::filesystem::path util_canonical_path(std::filesystem::path const &path) {
  namespace fs = std::filesystem;
  fs::path const abs{ fs::absolute(path) };
  std::error_code ec;
  fs::path const canonical{ fs::weakly_canonical(abs, ec) };
  return ec ? abs : canonical;
}

std::string util_flatten_script_with_semicolons(std::string_view script) {
  if (script.empty()) { return {}; }

  std::string result;
  result.reserve(script.size());

  bool need_semicolon{ false };  // True if we need to emit "; " before next content
  bool in_whitespace{ false };   // True if we've seen whitespace since last content
  bool have_content{ false };    // True if current line has any content

  for (std::size_t i{ 0 }; i < script.size(); ++i) {
    char const c{ script[i] };

    if (c == '\n' || c == '\r') {
      if (i + 1 < script.size()) {
        char const next{ script[i + 1] };
        if ((c == '\r' && next == '\n') || (c == '\n' && next == '\r')) { ++i; }
      }

      if (have_content) {
        need_semicolon = true;
        have_content = false;
      }
      in_whitespace = false;
    } else if (c == ' ' || c == '\t') {
      // Mark that we're in whitespace, but don't emit yet
      if (have_content) { in_whitespace = true; }
    } else {
      if (need_semicolon) {
        result += "; ";
        need_semicolon = false;
        in_whitespace = false;
      } else if (in_whitespace) {
        result += ' ';
        in_whitespace = false;
      }
      result += c;
      have_content = true;
    }
  }

  // Trim trailing "; " and whitespace
  while (!result.empty()) {
    std::size_t const len{ result.size() };
    if (len >= 2 && result[len - 2] == ';' && result[len - 1] == ' ') {
      result.resize(len - 2);
    } else if (result.back() == ' ' || result.back() == '\t') {
      result.pop_back();
    } else {
      break;
    }
  }

  return result;
}

namespace {

// Normalize path separators to forward slashes for comparison
std::string normalize_slashes(std::string_view path) {
  std::string result{ path };
  for (char &c : result) {
    if (c == '\\') { c = '/'; }
  }
  return result;
}

// Check if token ends with product path (after a path separator)
// Returns product name if matched, empty string otherwise
std::string match_product_suffix(std::string_view token, product_map_t const &products) {
  if (products.empty()) { return {}; }

  std::string const normalized_token{ normalize_slashes(token) };

  for (auto const &[name, path] : products) {
    if (path.empty()) { continue; }

    std::string const normalized_path{ normalize_slashes(path) };

    // Check for exact match
    if (normalized_token == normalized_path) { return name; }

    // Check for suffix match: token ends with /path or \path
    std::string const suffix{ "/" + normalized_path };
    if (normalized_token.size() > suffix.size() &&
        normalized_token.substr(normalized_token.size() - suffix.size()) == suffix) {
      return name;
    }
  }

  return {};
}

// Simplify a single path-like value (used for both standalone tokens and RHS of key=value)
// Returns simplified string, or empty if no simplification possible
std::string simplify_path_value(std::string_view value,
                                std::string const &normalized_cache_root,
                                product_map_t const &products) {
  // Try product match first
  std::string const product_name{ match_product_suffix(value, products) };
  if (!product_name.empty()) { return product_name; }

  // Try cache_root prefix detection
  if (!normalized_cache_root.empty()) {
    std::string const normalized_value{ normalize_slashes(value) };

    bool const is_cache_path{ [&] {
      if (normalized_value.size() > normalized_cache_root.size() &&
          normalized_value.substr(0, normalized_cache_root.size()) ==
              normalized_cache_root) {
        return normalized_value[normalized_cache_root.size()] == '/';
      }
      return normalized_value == normalized_cache_root;
    }() };

    if (is_cache_path) {
      std::filesystem::path value_path{ value };
      // Handle trailing slash: /foo/bar/ has empty filename(), use parent's filename
      if (value_path.filename().empty()) { value_path = value_path.parent_path(); }
      return value_path.filename().string();
    }
  }

  return {};
}

}  // namespace

std::string util_simplify_cache_paths(std::string_view command,
                                      std::filesystem::path const &cache_root,
                                      product_map_t const &products) {
  if (command.empty()) { return std::string{ command }; }

  std::string const cache_root_str{ cache_root.empty() ? "" : cache_root.string() };
  std::string const normalized_cache_root{ normalize_slashes(cache_root_str) };
  std::string result;
  result.reserve(command.size());

  auto const is_separator = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ';';
  };

  std::size_t pos{ 0 };
  while (pos < command.size()) {
    while (pos < command.size() && is_separator(command[pos])) {
      result += command[pos];
      ++pos;
    }

    if (pos >= command.size()) { break; }

    // Extract token (sequence until separator)
    std::size_t const token_start{ pos };
    while (pos < command.size() && !is_separator(command[pos])) { ++pos; }

    std::string_view const token{ command.data() + token_start, pos - token_start };

    // Check for key=value pattern and process RHS separately
    auto const eq_pos{ token.find('=') };
    if (eq_pos != std::string_view::npos && eq_pos > 0 && eq_pos < token.size() - 1) {
      std::string_view const key{ token.substr(0, eq_pos + 1) };  // Include '='
      std::string_view const value{ token.substr(eq_pos + 1) };

      if (std::string const simplified{
              simplify_path_value(value, normalized_cache_root, products) };
          !simplified.empty()) {
        result.append(key.data(), key.size());
        result += simplified;
        continue;
      }
    }

    // Try to simplify the whole token
    if (std::string const simplified{
            simplify_path_value(token, normalized_cache_root, products) };
        !simplified.empty()) {
      result += simplified;
      continue;
    }

    result.append(token.data(), token.size());
  }

  return result;
}

std::optional<parsed_archive_filename> util_parse_archive_filename(std::string_view stem) {
  auto const at_pos{ stem.find('@') };
  if (at_pos == std::string_view::npos) { return std::nullopt; }

  // From '@', find the next '-' after the revision
  auto const after_at{ stem.substr(at_pos + 1) };
  auto const dash_pos{ after_at.find('-') };
  if (dash_pos == std::string_view::npos) { return std::nullopt; }

  auto const identity_end{ at_pos + 1 + dash_pos };
  std::string const identity{ stem.substr(0, identity_end) };
  std::string_view remaining{ stem.substr(identity_end + 1) };

  // remaining = platform-arch-blake3-hash_prefix
  auto split_next = [&]() -> std::string {
    auto const pos{ remaining.find('-') };
    if (pos == std::string_view::npos) {
      std::string result{ remaining };
      remaining = {};
      return result;
    }
    std::string result{ remaining.substr(0, pos) };
    remaining = remaining.substr(pos + 1);
    return result;
  };

  std::string const platform{ split_next() };
  std::string const arch{ split_next() };
  std::string const blake3_tag{ split_next() };
  std::string const hash_prefix{ std::string(remaining) };

  if (platform.empty() || arch.empty() || blake3_tag != "blake3" || hash_prefix.empty()) {
    return std::nullopt;
  }

  return parsed_archive_filename{ identity, platform, arch, hash_prefix };
}

bool util_platform_matches(std::vector<std::string> const &constraints,
                           std::string_view target_os,
                           std::string_view target_arch) {
  if (constraints.empty()) { return true; }
  std::string const os_arch{ std::string(target_os) + "-" + std::string(target_arch) };
  for (auto const &c : constraints) {
    if (c == target_os || c == os_arch) { return true; }
  }
  return false;
}

std::vector<std::string> util_platform_intersect(std::vector<std::string> const &a,
                                                 std::vector<std::string> const &b) {
  if (a.empty()) { return b; }
  if (b.empty()) { return a; }
  std::vector<std::string> result;
  for (auto const &v : a) {
    for (auto const &w : b) {
      if (v == w) {
        result.push_back(v);
        break;
      }
    }
  }
  return result;
}

bool util_platform_matches_platform_id(std::vector<std::string> const &constraints,
                                       platform_id plat) {
  if (constraints.empty()) { return true; }
  for (auto const &c : constraints) {
    // Extract OS portion (before first '-' or entire string)
    auto const dash{ c.find('-') };
    std::string_view const os{ dash != std::string::npos
                                   ? std::string_view(c).substr(0, dash)
                                   : std::string_view(c) };
    if (plat == platform_id::POSIX && (os == "darwin" || os == "linux")) { return true; }
    if (plat == platform_id::WINDOWS && os == "windows") { return true; }
  }
  return false;
}

std::vector<platform_id> util_parse_platform_flag(std::string const &value) {
  if (value.empty()) { return { platform::native() }; }
  if (value == "posix") { return { platform_id::POSIX }; }
  if (value == "windows") { return { platform_id::WINDOWS }; }
  if (value == "all") { return { platform_id::POSIX, platform_id::WINDOWS }; }
  throw std::runtime_error("invalid --platform value '" + value +
                           "': expected posix, windows, or all");
}

scoped_path_cleanup::scoped_path_cleanup(std::filesystem::path path)
    : path_{ std::move(path) } {}

scoped_path_cleanup::~scoped_path_cleanup() { cleanup(); }

void scoped_path_cleanup::reset(std::filesystem::path path) {
  cleanup();
  path_ = std::move(path);
}

void scoped_path_cleanup::cleanup() {
  if (path_.empty()) { return; }
  std::error_code ec;
  std::filesystem::remove(path_, ec);
  path_.clear();
}

}  // namespace envy
