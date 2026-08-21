#include "envy_release.h"

#include "doctest.h"

#include <algorithm>
// doctest stringifies a failing CHECK's operands via operator<<. MSVC defines the
// string_view overload in <string_view> but only forward-declares basic_ostream there, so
// a string_view operand needs the complete type or instantiation fails.
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

// --- upstream location constants ---

TEST_CASE("release URLs derive from one upstream repo") {
  // Guards the one-line-relocation property: both constants share a base, so moving the
  // project to a new org means editing only ENVY_UPSTREAM_REPO_URL.
  constexpr std::string_view kDownloadSuffix{ "/releases/download" };
  constexpr std::string_view kLatestSuffix{ "/releases/latest" };
  REQUIRE(envy::kEnvyReleaseDownloadUrl.ends_with(kDownloadSuffix));
  REQUIRE(envy::kEnvyReleaseLatestUrl.ends_with(kLatestSuffix));

  auto const base{ envy::kEnvyReleaseDownloadUrl.substr(
      0,
      envy::kEnvyReleaseDownloadUrl.size() - kDownloadSuffix.size()) };
  CHECK(envy::kEnvyReleaseLatestUrl.starts_with(base));
  CHECK_FALSE(base.empty());
}

// --- envy_release_validate_mirror ---

TEST_CASE("envy_release_validate_mirror: ordinary mirrors accepted") {
  envy::envy_release_validate_mirror("https://github.com/org/envy/releases/download", "t");
  envy::envy_release_validate_mirror("s3://my-envy-mirror/releases", "t");
  envy::envy_release_validate_mirror("file:///tmp/releases", "t");
  // Characters that are fine in every consumer: query strings, ports, tildes, percent.
  envy::envy_release_validate_mirror("https://h:8443/a-b_c.d~e%20f?x=1&y=2", "t");
}

TEST_CASE("envy_release_validate_mirror: newline injection rejected") {
  // Would otherwise append arbitrary directives to envy.lua via the manifest stamp.
  CHECK_THROWS_AS(
      envy::envy_release_validate_mirror("https://x\"\n-- @envy version \"9.9.9", "t"),
      std::runtime_error);
  CHECK_THROWS_AS(envy::envy_release_validate_mirror("https://x\ny", "t"),
                  std::runtime_error);
  CHECK_THROWS_AS(envy::envy_release_validate_mirror("https://x\ry", "t"),
                  std::runtime_error);
}

TEST_CASE("envy_release_validate_mirror: quote and backslash rejected") {
  // The directive and the shell/batch assignments are all double-quoted.
  CHECK_THROWS_AS(envy::envy_release_validate_mirror("https://x\"y", "t"),
                  std::runtime_error);
  CHECK_THROWS_AS(envy::envy_release_validate_mirror("https://x\\y", "t"),
                  std::runtime_error);
}

TEST_CASE("envy_release_validate_mirror: batch delayed-expansion bang rejected") {
  // envy.bat runs under EnableDelayedExpansion, where `!` is a variable delimiter.
  CHECK_THROWS_AS(envy::envy_release_validate_mirror("https://x/a!b", "t"),
                  std::runtime_error);
}

TEST_CASE("envy_release_validate_mirror: control characters and empty rejected") {
  CHECK_THROWS_AS(envy::envy_release_validate_mirror("https://x\ty", "t"),
                  std::runtime_error);
  CHECK_THROWS_AS(envy::envy_release_validate_mirror(std::string_view{ "a\0b", 3 }, "t"),
                  std::runtime_error);
  CHECK_THROWS_AS(envy::envy_release_validate_mirror("", "t"), std::runtime_error);
}

TEST_CASE("envy_release_validate_mirror: op label appears in the error") {
  try {
    envy::envy_release_validate_mirror("bad\nvalue", "init");
    FAIL("expected throw");
  } catch (std::runtime_error const &e) {
    CHECK(std::string{ e.what() }.starts_with("init: "));
  }
}

// --- envy_release_version_is_valid ---

TEST_CASE("envy_release_version_is_valid: normal version") {
  CHECK(envy::envy_release_version_is_valid("1.2.3"));
}

TEST_CASE("envy_release_version_is_valid: version with pre-release suffix") {
  CHECK(envy::envy_release_version_is_valid("1.2.3-beta.1"));
}

TEST_CASE("envy_release_version_is_valid: version with underscore") {
  CHECK(envy::envy_release_version_is_valid("1_2_3"));
}

TEST_CASE("envy_release_version_is_valid: empty string rejected") {
  CHECK_FALSE(envy::envy_release_version_is_valid(""));
}

TEST_CASE("envy_release_version_is_valid: path traversal rejected") {
  CHECK_FALSE(envy::envy_release_version_is_valid("../../../etc/passwd"));
}

TEST_CASE("envy_release_version_is_valid: bare '.' and '..' rejected") {
  // Both clear the character filter, and both name a directory instead of a release:
  // '<cache>/envy/../envy' exists, so the re-exec fast path would try to exec it.
  CHECK_FALSE(envy::envy_release_version_is_valid("."));
  CHECK_FALSE(envy::envy_release_version_is_valid(".."));
  // Only the whole component is special; a dot-led version is still a version.
  CHECK(envy::envy_release_version_is_valid("..1"));
  CHECK(envy::envy_release_version_is_valid("1.."));
  CHECK(envy::envy_release_version_is_valid("..."));
}

TEST_CASE("envy_release_version_is_valid: slash rejected") {
  CHECK_FALSE(envy::envy_release_version_is_valid("1.2.3/evil"));
}

TEST_CASE("envy_release_version_is_valid: backslash rejected") {
  CHECK_FALSE(envy::envy_release_version_is_valid("1.2.3\\evil"));
}

TEST_CASE("envy_release_version_is_valid: space rejected") {
  CHECK_FALSE(envy::envy_release_version_is_valid("1.2.3 ; rm -rf /"));
}

TEST_CASE("envy_release_version_is_valid: non-ASCII rejected regardless of locale") {
  // Version becomes the envy/<version> cache path component; high-bit UTF-8 bytes
  // must be rejected (std::isalnum could accept them under a non-"C" locale).
  CHECK_FALSE(envy::envy_release_version_is_valid("1.2.3-caf\xc3\xa9"));  // "café"
  CHECK_FALSE(envy::envy_release_version_is_valid("\xe4\xbd\xa0"));       // "你"
}

TEST_CASE("envy_release_version_is_valid: null byte rejected") {
  CHECK_FALSE(envy::envy_release_version_is_valid(std::string_view{ "1.2\0.3", 6 }));
}

// --- envy_release_archive_ext / envy_release_archive_name ---

// The extension is keyed on the target os, never the host: mirroring the full release set
// from any one machine has to name the windows artifacts correctly.

TEST_CASE("envy_release_archive_ext: windows is zip, posix is tar.gz") {
  CHECK(envy::envy_release_archive_ext("windows") == ".zip");
  CHECK(envy::envy_release_archive_ext("darwin") == ".tar.gz");
  CHECK(envy::envy_release_archive_ext("linux") == ".tar.gz");
}

TEST_CASE("envy_release_archive_name: every published release target") {
  CHECK(envy::envy_release_archive_name("darwin", "arm64") == "envy-darwin-arm64.tar.gz");
  CHECK(envy::envy_release_archive_name("darwin", "x86_64") ==
        "envy-darwin-x86_64.tar.gz");
  CHECK(envy::envy_release_archive_name("linux", "arm64") == "envy-linux-arm64.tar.gz");
  CHECK(envy::envy_release_archive_name("linux", "x86_64") == "envy-linux-x86_64.tar.gz");
  CHECK(envy::envy_release_archive_name("windows", "arm64") == "envy-windows-arm64.zip");
  CHECK(envy::envy_release_archive_name("windows", "x86_64") == "envy-windows-x86_64.zip");
}

TEST_CASE("kEnvyReleaseTargets: matches the release workflow's asset matrix") {
  // Names must stay byte-identical to .github/workflows/release.yml or mirrors 404.
  CHECK(envy::kEnvyReleaseTargets.size() == 6);

  auto const has{ [](std::string_view os, std::string_view arch) {
    return std::ranges::any_of(envy::kEnvyReleaseTargets, [&](auto const &t) {
      return t.os == os && t.arch == arch;
    });
  } };
  CHECK(has("darwin", "arm64"));
  CHECK(has("darwin", "x86_64"));
  CHECK(has("linux", "arm64"));
  CHECK(has("linux", "x86_64"));
  CHECK(has("windows", "arm64"));
  CHECK(has("windows", "x86_64"));
}

// --- envy_release_url ---

// Derived from the constant, not a second copy of the URL: the point of centralizing it is
// that relocating the project does not require editing tests too.
TEST_CASE("envy_release_url: default mirror darwin arm64") {
  auto const url{
    envy::envy_release_url(envy::kEnvyReleaseDownloadUrl, "1.2.3", "darwin", "arm64")
  };
  CHECK(url ==
        std::string{ envy::kEnvyReleaseDownloadUrl } + "/v1.2.3/envy-darwin-arm64.tar.gz");
}

TEST_CASE("envy_release_url: linux x86_64") {
  auto const url{
    envy::envy_release_url(envy::kEnvyReleaseDownloadUrl, "2.0.0", "linux", "x86_64")
  };
  CHECK(url ==
        std::string{ envy::kEnvyReleaseDownloadUrl } + "/v2.0.0/envy-linux-x86_64.tar.gz");
}

TEST_CASE("envy_release_url: windows names a zip from any host") {
  auto const url{ envy::envy_release_url("https://my-mirror.example.com/envy",
                                         "2.0.0",
                                         "windows",
                                         "x86_64") };
  CHECK(url == "https://my-mirror.example.com/envy/v2.0.0/envy-windows-x86_64.zip");
}

TEST_CASE("envy_release_url: custom mirror") {
  auto const url{ envy::envy_release_url("https://my-mirror.example.com/envy",
                                         "2.0.0",
                                         "linux",
                                         "x86_64") };
  CHECK(url == "https://my-mirror.example.com/envy/v2.0.0/envy-linux-x86_64.tar.gz");
}

TEST_CASE("envy_release_url: file mirror") {
  auto const url{
    envy::envy_release_url("file:///tmp/releases", "1.0.0", "darwin", "arm64")
  };
  CHECK(url == "file:///tmp/releases/v1.0.0/envy-darwin-arm64.tar.gz");
}

TEST_CASE("envy_release_url: s3 mirror") {
  auto const url{
    envy::envy_release_url("s3://my-bucket/envy-releases", "3.1.0", "linux", "arm64")
  };
  CHECK(url == "s3://my-bucket/envy-releases/v3.1.0/envy-linux-arm64.tar.gz");
}

TEST_CASE("envy_release_url: trailing slash on mirror produces double slash") {
  // Callers must strip trailing slashes: for s3:// this would be a distinct (missing) key.
  auto const url{
    envy::envy_release_url("https://mirror.example.com/", "1.0.0", "darwin", "arm64")
  };
  CHECK(url == "https://mirror.example.com//v1.0.0/envy-darwin-arm64.tar.gz");
}

// --- checksum manifest ---

TEST_CASE("envy_release_sums_url: hangs off the same versioned prefix as the archives") {
  CHECK(envy::envy_release_sums_url("https://mirror.example.com/rel", "1.2.3") ==
        "https://mirror.example.com/rel/v1.2.3/SHA256SUMS");
  CHECK(envy::envy_release_sums_url("s3://b/prefix", "0.9.0") ==
        "s3://b/prefix/v0.9.0/SHA256SUMS");
}

TEST_CASE("envy_release_sha256_hex_is_valid: exactly 64 hex digits, either case") {
  std::string const lower(64, 'a');
  std::string const upper(64, 'F');
  CHECK(envy::envy_release_sha256_hex_is_valid(lower));
  CHECK(envy::envy_release_sha256_hex_is_valid(upper));
  CHECK(envy::envy_release_sha256_hex_is_valid(
      "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"));

  // A truncated pin must be rejected outright rather than compared on its prefix: a
  // one-digit pin would otherwise "verify" one sixteenth of the digests in existence.
  CHECK_FALSE(envy::envy_release_sha256_hex_is_valid(""));
  CHECK_FALSE(envy::envy_release_sha256_hex_is_valid(std::string(63, 'a')));
  CHECK_FALSE(envy::envy_release_sha256_hex_is_valid(std::string(65, 'a')));
  CHECK_FALSE(envy::envy_release_sha256_hex_is_valid(std::string(63, 'a') + "g"));
  CHECK_FALSE(envy::envy_release_sha256_hex_is_valid(std::string(63, 'a') + " "));
}

namespace {

// Two entries in the exact shape `sha256sum *.tar.gz *.zip > SHA256SUMS` produces.
constexpr std::string_view kSums{
  "1111111111111111111111111111111111111111111111111111111111111111  "
  "envy-linux-x86_64.tar.gz\n"
  "2222222222222222222222222222222222222222222222222222222222222222  "
  "envy-windows-x86_64.zip\n"
};

}  // namespace

TEST_CASE("envy_release_sums_lookup: finds each artifact") {
  CHECK(envy::envy_release_sums_lookup(kSums, "envy-linux-x86_64.tar.gz") ==
        std::string(64, '1'));
  CHECK(envy::envy_release_sums_lookup(kSums, "envy-windows-x86_64.zip") ==
        std::string(64, '2'));
}

TEST_CASE("envy_release_sums_lookup: absent artifact yields nullopt") {
  // A sums file that does not describe the running platform must fail the bootstrap, not
  // fall back to skipping verification.
  CHECK_FALSE(
      envy::envy_release_sums_lookup(kSums, "envy-darwin-arm64.tar.gz").has_value());
  CHECK_FALSE(envy::envy_release_sums_lookup("", "envy-linux-x86_64.tar.gz").has_value());
}

TEST_CASE("envy_release_sums_lookup: empty artifact name never matches") {
  // Otherwise it matches the empty tail of a malformed line and returns that line's hash,
  // attesting an archive against an unrelated digest.
  CHECK_FALSE(envy::envy_release_sums_lookup(kSums, "").has_value());
}

TEST_CASE("envy_release_sums_lookup: tolerates CRLF, binary marker, and tabs") {
  // A Windows producer emits CRLF; `sha256sum -b` prefixes the name with '*'. Neither is
  // worth failing a release over.
  constexpr std::string_view kOdd{
    "3333333333333333333333333333333333333333333333333333333333333333 "
    "*envy-windows-arm64.zip\r\n"
    "\t4444444444444444444444444444444444444444444444444444444444444444\t"
    "envy-linux-arm64.tar.gz\r\n"
  };
  CHECK(envy::envy_release_sums_lookup(kOdd, "envy-windows-arm64.zip") ==
        std::string(64, '3'));
  CHECK(envy::envy_release_sums_lookup(kOdd, "envy-linux-arm64.tar.gz") ==
        std::string(64, '4'));
}

TEST_CASE("envy_release_sums_lookup: last line needs no trailing newline") {
  CHECK(envy::envy_release_sums_lookup(std::string(64, 'a') + "  envy-linux-arm64.tar.gz",
                                       "envy-linux-arm64.tar.gz") == std::string(64, 'a'));
}

TEST_CASE("envy_release_sums_lookup: skips lines whose hash is not a sha256") {
  // Guards against a GPG-signed or otherwise decorated sums file donating a short token as
  // if it were a digest.
  constexpr std::string_view kNoise{
    "-----BEGIN PGP SIGNED MESSAGE-----\n"
    "Hash: SHA256\n"
    "\n"
    "deadbeef  envy-linux-arm64.tar.gz\n"
    "5555555555555555555555555555555555555555555555555555555555555555  "
    "envy-linux-arm64.tar.gz\n"
  };
  CHECK(envy::envy_release_sums_lookup(kNoise, "envy-linux-arm64.tar.gz") ==
        std::string(64, '5'));
}
