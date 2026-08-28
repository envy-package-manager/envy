#include "envy_release.h"

#include "sha256.h"
#include "util.h"

#include "semver.hpp"

#include <sstream>
#include <stdexcept>

namespace envy {

namespace {

// Both sides are meant to be 64-digit hex, so comparing digit values is an exact match
// that is also case-insensitive: `sha256sum` emits lowercase while `certutil` and
// `Get-FileHash` emit uppercase, and a hand-pasted pin can be either. A non-hex digit on
// either side compares unequal, so a garbage pin surfaces as a mismatch, not a false pass.
bool hex_equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) { return false; }
  for (size_t i{ 0 }; i < a.size(); ++i) {
    int const x{ util_hex_char_to_int(a[i]) };
    if (x < 0 || x != util_hex_char_to_int(b[i])) { return false; }
  }
  return true;
}

}  // namespace

bool envy_release_version_is_valid(std::string_view version) {
  if (version.empty()) { return false; }
  // '.' and '..' clear the character filter but name a directory rather than a release:
  // '<cache>/envy/../envy' exists, and reaching execve with it merely fails.
  if (version == "." || version == "..") { return false; }
  // ASCII-only: version becomes the envy/<version> cache path component.
  for (char c : version) {
    if (!util_ascii_is_alnum(c) && c != '.' && c != '-' && c != '_') { return false; }
  }
  return true;
}

bool envy_release_version_less(std::string_view a, std::string_view b) {
  semver::version<> va, vb;
  if (!semver::parse(a, va) || !semver::parse(b, vb)) { return false; }
  return va < vb;
}

void envy_release_validate_mirror(std::string_view mirror, std::string_view op) {
  auto const reject{ [&](std::string_view what) {
    std::ostringstream msg;
    msg << op << ": mirror contains " << what
        << ", which cannot be represented in a manifest directive or bootstrap script: '"
        << mirror << "'";
    throw std::runtime_error(msg.str());
  } };

  if (mirror.empty()) { reject("nothing (empty value)"); }

  for (unsigned char const c : mirror) {
    switch (c) {
      case '"': reject("a double quote"); break;
      case '\\': reject("a backslash"); break;
      case '!': reject("an exclamation mark (batch delayed expansion)"); break;
      case '\n': reject("a newline"); break;
      case '\r': reject("a carriage return"); break;
      default:
        if (c < 0x20 || c == 0x7F) { reject("a control character"); }
        break;
    }
  }
}

std::string_view envy_release_archive_ext(std::string_view os) {
  return os == "windows" ? ".zip" : ".tar.gz";
}

std::string envy_release_archive_name(std::string_view os, std::string_view arch) {
  std::ostringstream ss;
  ss << "envy-" << os << '-' << arch << envy_release_archive_ext(os);
  return ss.str();
}

std::string envy_release_url(std::string_view mirror_base,
                             std::string_view version,
                             std::string_view os,
                             std::string_view arch) {
  std::ostringstream ss;
  ss << mirror_base << "/v" << version << '/' << envy_release_archive_name(os, arch);
  return ss.str();
}

std::string envy_release_sums_url(std::string_view mirror_base, std::string_view version) {
  std::ostringstream ss;
  ss << mirror_base << "/v" << version << '/' << kEnvyReleaseSumsFile;
  return ss.str();
}

bool envy_release_sha256_hex_is_valid(std::string_view hex) {
  if (hex.size() != 64) { return false; }
  for (char const c : hex) {
    if (util_hex_char_to_int(c) < 0) { return false; }
  }
  return true;
}

std::optional<std::string> envy_release_sums_lookup(std::string_view sums_text,
                                                    std::string_view artifact_name) {
  // An empty name would match the empty remainder of a malformed line and hand back that
  // line's hash, attesting an archive against an unrelated digest.
  if (artifact_name.empty()) { return std::nullopt; }

  auto const is_space{ [](char c) { return c == ' ' || c == '\t'; } };

  for (size_t pos{ 0 }; pos < sums_text.size();) {
    auto const eol{ sums_text.find('\n', pos) };
    auto line{
      sums_text.substr(pos, (eol == std::string_view::npos ? sums_text.size() : eol) - pos)
    };
    pos = (eol == std::string_view::npos) ? sums_text.size() : eol + 1;

    if (line.ends_with('\r')) { line.remove_suffix(1); }  // CRLF from a Windows producer
    while (!line.empty() && is_space(line.front())) { line.remove_prefix(1); }

    auto const sep{ line.find_first_of(" \t") };
    if (sep == std::string_view::npos) { continue; }

    auto const hash{ line.substr(0, sep) };
    if (!envy_release_sha256_hex_is_valid(hash)) { continue; }

    auto name{ line.substr(sep) };
    while (!name.empty() && is_space(name.front())) { name.remove_prefix(1); }
    // `sha256sum -b` and BSD's `-` mode prefix the name with '*' for binary mode.
    if (name.starts_with('*')) { name.remove_prefix(1); }

    if (name == artifact_name) { return std::string{ hash }; }
  }

  return std::nullopt;
}

std::string envy_release_load_sums(std::filesystem::path const &sums_file,
                                   std::optional<std::string> const &pinned_hex,
                                   std::string_view op) {
  auto const bytes{ util_load_file(sums_file) };

  if (pinned_hex) {
    auto const actual{ sha256(sums_file) };
    auto const actual_hex{ util_bytes_to_hex(actual.data(), actual.size()) };
    if (!hex_equal(actual_hex, *pinned_hex)) {
      std::ostringstream msg;
      msg << op << ": " << kEnvyReleaseSumsFile
          << " does not match the pinned '@envy sha256sums': expected " << *pinned_hex
          << ", got " << actual_hex
          << ". The mirror is serving a different release manifest than the one this "
             "project pinned -- update the pin deliberately, do not remove it.";
      throw std::runtime_error(msg.str());
    }
  }

  return { reinterpret_cast<char const *>(bytes.data()), bytes.size() };
}

void envy_release_verify_artifact(std::filesystem::path const &artifact,
                                  std::string_view artifact_name,
                                  std::string_view sums_text,
                                  std::string_view op,
                                  byte_progress_cb_t const &progress) {
  auto const expected{ envy_release_sums_lookup(sums_text, artifact_name) };
  if (!expected) {
    std::ostringstream msg;
    msg << op << ": " << kEnvyReleaseSumsFile << " lists no entry for " << artifact_name;
    throw std::runtime_error(msg.str());
  }

  auto const actual{ sha256(artifact, progress) };
  auto const actual_hex{ util_bytes_to_hex(actual.data(), actual.size()) };
  if (!hex_equal(actual_hex, *expected)) {
    std::ostringstream msg;
    msg << op << ": " << artifact_name << " failed attestation: " << kEnvyReleaseSumsFile
        << " says " << *expected << ", downloaded bytes hash to " << actual_hex;
    throw std::runtime_error(msg.str());
  }
}

}  // namespace envy
