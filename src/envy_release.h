#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace envy {

// Naming and addressing for envy's own release artifacts. Shared by the self-upgrade path
// (which consumes one artifact) and `envy mirror-envy` (which republishes all of them), so
// both agree on what a release is called and where it lives.

// Single source of truth for where envy itself is published. Relocating the project to a
// different GitHub org is a one-line edit here: every derived URL falls out below, and
// both bootstrap scripts are stamped from these constants instead of carrying copies.
#define ENVY_UPSTREAM_REPO_URL "https://github.com/envy-package-manager/envy"

// Default mirror base: release assets hang off this as /v<version>/<archive name>.
inline constexpr std::string_view kEnvyReleaseDownloadUrl{ ENVY_UPSTREAM_REPO_URL
                                                           "/releases/download" };

// Resolves the newest published version via its redirect to the tag. GitHub serves no
// `latest` object, so this is the only way to ask it; a custom mirror answers with a
// `latest` file instead (see kMirrorLatestFile).
inline constexpr std::string_view kEnvyReleaseLatestUrl{ ENVY_UPSTREAM_REPO_URL
                                                         "/releases/latest" };

// Concatenation above happens in the preprocessor; undef so the macro does not leak into
// every translation unit that includes this header.
#undef ENVY_UPSTREAM_REPO_URL

// First release that understands '@envy cache-local', 'cache-mode' and 'state-dir'.
//
// Load-bearing, not informational. An older envy drops unknown directive keys silently, so
// a manifest that pins one and also names a local cache would have it resolve the *shared*
// cache and exit 0 -- the project asks for a hermetic tree and gets packages in the user's
// home instead. Both launchers are stamped with this and refuse to hand a manifest using
// the new directives to a binary below it, and reexec refuses the same downgrade.
inline constexpr std::string_view kEnvyMinDirectiveVersion{ "0.2.0" };

// Checksum manifest published beside the archives of every release, one line per artifact
// in `sha256sum` output format. Bootstrap and re-exec attest a download against it, and a
// manifest's `@envy sha256sums` pins this file's own hash -- one pin covering all six
// platforms, since a cross-platform manifest cannot reasonably carry six archive hashes.
//
// `envy mirror-envy` republishes it byte-for-byte rather than regenerating it, so the pin
// is mirror-independent: the same `@envy sha256sums` value verifies against upstream and
// against every mirror downstream of it.
inline constexpr std::string_view kEnvyReleaseSumsFile{ "SHA256SUMS" };

struct envy_release_target {
  std::string_view os;
  std::string_view arch;
};

// Every artifact published per envy release; see .github/workflows/release.yml. Ordered
// for deterministic output when mirroring the whole set.
inline constexpr std::array<envy_release_target, 6> kEnvyReleaseTargets{ {
    { "darwin", "arm64" },
    { "darwin", "x86_64" },
    { "linux", "arm64" },
    { "linux", "x86_64" },
    { "windows", "arm64" },
    { "windows", "x86_64" },
} };

// A version becomes the envy/<version> cache path component, so reject anything that could
// escape it or confuse a shell.
bool envy_release_version_is_valid(std::string_view version);

// True when `a` names an earlier release than `b`. Both must parse as MAJOR.MINOR.PATCH;
// anything that does not is reported as *not* less, so a nonstandard version string never
// trips a version gate on the strength of a parse failure alone.
bool envy_release_version_less(std::string_view a, std::string_view b);

// A mirror gets written verbatim into a quoted `-- @envy mirror "..."` manifest directive,
// which both bootstrap scripts then parse back out into a shell/batch variable. Characters
// that would need escaping are rejected outright rather than escaped: a newline would
// inject extra directives into the manifest, and the batch directive parser cannot
// represent `\"` or `!` at all (see docs/envy-init.md), so escaping would yield a manifest
// that works on POSIX and silently breaks on Windows. Throws std::runtime_error naming the
// bad byte.
void envy_release_validate_mirror(std::string_view mirror, std::string_view op);

// Keyed on the target os rather than the host, so a posix host can name (and mirror) the
// windows artifacts.
std::string_view envy_release_archive_ext(std::string_view os);
std::string envy_release_archive_name(std::string_view os, std::string_view arch);

// <mirror_base>/v<version>/<archive name>. mirror_base must not carry a trailing slash:
// for an s3:// mirror the resulting double slash is a distinct, nonexistent key.
std::string envy_release_url(std::string_view mirror_base,
                             std::string_view version,
                             std::string_view os,
                             std::string_view arch);

// <mirror_base>/v<version>/SHA256SUMS, same trailing-slash rule as above.
std::string envy_release_sums_url(std::string_view mirror_base, std::string_view version);

// Exactly 64 hex digits, either case. Anything else in `@envy sha256sums` is a typo or a
// truncation, and a truncated pin must not silently weaken the check.
bool envy_release_sha256_hex_is_valid(std::string_view hex);

// Pull one artifact's hash out of SHA256SUMS text. Accepts `sha256sum` output verbatim,
// including the `*name` binary-mode marker and CRLF line endings. nullopt if the name is
// absent, which for a release archive means the mirror is serving a sums file that does
// not describe the platform being bootstrapped.
std::optional<std::string> envy_release_sums_lookup(std::string_view sums_text,
                                                    std::string_view artifact_name);

// Read a downloaded SHA256SUMS and return its text, first checking the file's own hash
// against a manifest's `@envy sha256sums` when one is pinned. This is the step that
// anchors the chain: everything downstream trusts the sums text, so a pin mismatch must
// abort before any artifact is checked against it. Throws std::runtime_error, prefixed op.
std::string envy_release_load_sums(std::filesystem::path const &sums_file,
                                   std::optional<std::string> const &pinned_hex,
                                   std::string_view op);

// Verify one downloaded artifact against already-loaded sums text. Throws
// std::runtime_error prefixed with op, naming the artifact and both hashes, if the entry
// is missing or the bytes disagree.
void envy_release_verify_artifact(std::filesystem::path const &artifact,
                                  std::string_view artifact_name,
                                  std::string_view sums_text,
                                  std::string_view op);

}  // namespace envy
