#include "reexec.h"

#include "cache.h"
#include "envy_release.h"
#include "extract.h"
#include "fetch.h"
#include "platform.h"
#include "tui.h"
#include "tui_actions.h"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef ENVY_VERSION_STR
#error "ENVY_VERSION_STR must be defined by the build system"
#endif

namespace envy {

namespace {

std::string_view get_self_version() {
  if (auto const *v = std::getenv("ENVY_TEST_SELF_VERSION")) { return v; }
  return ENVY_VERSION_STR;
}

void make_executable([[maybe_unused]] std::filesystem::path const &path) {
#ifndef _WIN32
  std::error_code ec;
  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_exec |
                                   std::filesystem::perms::group_exec |
                                   std::filesystem::perms::others_exec,
                               std::filesystem::perm_options::add,
                               ec);
  if (ec) {
    tui::warn("reexec: failed to set executable permissions: %s", ec.message().c_str());
  }
#endif
}

void remove_quarantine([[maybe_unused]] std::filesystem::path const &path) {
#ifdef __APPLE__
  std::ostringstream cmd;
  cmd << "xattr -d com.apple.quarantine '" << path.string() << "' 2>/dev/null";
  std::system(cmd.str().c_str());
#endif
}

// Build child env: copy current env, add ENVY_REEXEC=1, strip ENVY_TEST_SELF_VERSION.
std::vector<std::string> build_child_env() {
  auto env{ platform::get_environment() };
  std::vector<std::string> result;
  result.reserve(env.size() + 1);
  bool found_reexec{ false };

  for (auto &entry : env) {
    if (entry.starts_with("ENVY_TEST_SELF_VERSION=")) { continue; }
    if (entry.starts_with("ENVY_REEXEC=")) {
      found_reexec = true;
      result.emplace_back("ENVY_REEXEC=1");
    } else {
      result.push_back(std::move(entry));
    }
  }

  if (!found_reexec) { result.emplace_back("ENVY_REEXEC=1"); }
  return result;
}

}  // namespace

std::vector<char *> reexec_argv_without(char **argv, std::string_view option) {
  std::vector<char *> out;

  for (char **p{ argv }; p && *p; ++p) {
    std::string_view const arg{ *p };
    // A separated value is the next word, so it goes too -- leaving it behind would hand
    // the child a bare version string as a positional argument.
    if (arg == option) {
      if (*(p + 1)) { ++p; }
      continue;
    }
    if (arg.starts_with(option) && arg.size() > option.size() &&
        arg[option.size()] == '=') {
      continue;
    }
    out.push_back(*p);
  }

  out.push_back(nullptr);
  return out;
}

std::vector<char *> reexec_child_argv(reexec_request const &request, char **argv) {
  std::vector<char *> acc;
  for (char **p{ argv }; p && *p; ++p) { acc.push_back(*p); }
  acc.push_back(nullptr);

  // One pass per dropped option, each reading the array the last one produced. Only the
  // pointer array is ever rebuilt; the strings stay argv's throughout.
  for (auto const &option : request.drop_options) {
    auto next{ reexec_argv_without(acc.data(), option) };
    acc.swap(next);
  }
  return acc;
}

int reexec_exec(reexec_request const &request, char **argv) {
  auto child_argv{ reexec_child_argv(request, argv) };  // not const: exec wants char **
  tui::info("reexec: switching to envy at %s", request.binary.string().c_str());

  // The child owns the terminal from here: POSIX replaces this process, Windows waits. The
  // rows come down first so its output starts clean and no final render paints them back.
  tui::sections_clear();
  tui::interactive_mode_guard const terminal_handoff;

  return platform::exec_process(request.binary, child_argv.data(), build_child_env());
}

std::string_view reexec_self_version() { return get_self_version(); }

reexec_decision reexec_should(std::string_view self_version,
                              std::optional<std::string> const &requested_version,
                              bool reexec_env_set,
                              bool no_reexec_env_set) {
  if (!requested_version) { return reexec_decision::PROCEED; }
  if (no_reexec_env_set) { return reexec_decision::PROCEED; }
  if (self_version == "0.0.0") { return reexec_decision::PROCEED; }
  if (reexec_env_set) { return reexec_decision::PROCEED; }
  if (self_version == *requested_version) { return reexec_decision::PROCEED; }
  return reexec_decision::REEXEC;
}

void reexec_if_needed(envy_meta const &meta,
                      cache_root_resolution const &resolved,
                      std::filesystem::path const &manifest_dir,
                      std::vector<std::string> drop_options) {
  // Consume and unset the loop guard if present
  bool const reexec_env_set{ std::getenv("ENVY_REEXEC") != nullptr };
  if (reexec_env_set) { platform::env_var_unset("ENVY_REEXEC"); }

  bool const no_reexec_env_set{ std::getenv("ENVY_NO_REEXEC") != nullptr };
  auto const self_ver{ get_self_version() };

  if (reexec_should(self_ver, meta.version, reexec_env_set, no_reexec_env_set) ==
      reexec_decision::PROCEED) {
    return;
  }

  auto const &version{ *meta.version };

  if (!envy_release_version_is_valid(version)) {
    throw std::runtime_error("reexec: invalid version string: " + version);
  }

  // An older envy silently ignores '@envy cache-local'/'cache-mode'/'state-dir' and would
  // resolve the shared cache for a manifest asking for a hermetic tree, exiting 0. Refuse
  // the downgrade rather than hand it a manifest it cannot read correctly.
  // 0.0.0 is a dev build, let through for the same reason reexec_should() lets a 0.0.0
  // self through: built from a working tree, so its support cannot be read off a version.
  //
  // A MARKER tier counts as much as the directives do: `envy cache --local` puts a project
  // on its own tree with no directive anywhere in the manifest, and an envy that predates
  // the markers cannot see that choice either -- it would read the same manifest, resolve
  // the shared cache, and exit 0.
  if ((meta.cache_local || meta.declared_cache_mode || meta.state_dir ||
       resolved.tier == cache_root_tier::MARKER) &&
      version != "0.0.0" && envy_release_version_less(version, kEnvyMinDirectiveVersion)) {
    throw std::runtime_error(
        "manifest resolves a cache mode from '@envy cache-local'/'cache-mode'/'state-dir' "
        "or an 'envy cache' marker, but pins '@envy version \"" +
        version + "\"', which predates them (added in " +
        std::string{ kEnvyMinDirectiveVersion } +
        "). That envy would silently use the shared cache. Raise or remove the version "
        "pin.");
  }

  // Fast path: the requested version may already be on disk. A local tree may borrow the
  // user's own copy rather than re-download one it already has; see envy_binary_candidates
  // for why that is read-only and why a sums pin turns it off.
  for (auto const &candidate :
       envy_binary_candidates(resolved,
                              resolve_user_wide_cache_root(std::nullopt),
                              version,
                              meta.sha256sums.has_value())) {
    if (std::filesystem::is_regular_file(candidate)) {
      throw reexec_request{ candidate, std::move(drop_options) };
    }
  }

  // Slow path: download to temp dir, re-exec from there.
  // The re-exec'd binary's own cache::ensure_envy() will install itself into cache.

  std::string_view mirror{ kEnvyReleaseDownloadUrl };
  if (char const *env_mirror = std::getenv("ENVY_MIRROR"); env_mirror) {
    mirror = env_mirror;
  } else if (meta.mirror) {
    mirror = *meta.mirror;
  }

  auto const url{
    envy_release_url(mirror, version, platform::os_name(), platform::arch_name())
  };
  tui::info("reexec: downloading envy %s from %s", version.c_str(), url.c_str());

  auto const pid{ platform::get_process_id() };
  auto const tmp_dir{ std::filesystem::temp_directory_path() /
                      ("envy-reexec-" + version + "-" + std::to_string(pid)) };
  std::filesystem::create_directories(tmp_dir);

  auto const archive_name{ envy_release_archive_name(platform::os_name(),
                                                     platform::arch_name()) };
  auto const archive_path{ tmp_dir / archive_name };

  // With a sums pin, SHA256SUMS is fetched alongside the archive rather than before it: it
  // is a few hundred bytes, so serializing the two only to fail fast on a bad pin costs a
  // round trip on every re-exec and saves nothing on the happy path.
  std::vector<fetch_request> requests{ fetch_request_from_url(url, archive_path) };
  std::vector<std::string> urls{ url };
  auto const sums_path{ tmp_dir / std::string{ kEnvyReleaseSumsFile } };
  if (meta.sha256sums) {
    auto sums_url{ envy_release_sums_url(mirror, version) };
    requests.push_back(fetch_request_from_url(sums_url, sums_path));
    urls.push_back(std::move(sums_url));
  }

  // Multi-megabyte archive: draw a bar rather than leaving the terminal silent.
  std::vector<std::string> labels;
  labels.reserve(requests.size());
  for (auto const &req : requests) {
    labels.push_back(
        std::visit([](auto const &r) { return r.destination.filename().string(); }, req));
  }

  auto const results{
    tui_actions::fetch_tracked(std::move(requests), "envy " + version, labels)
  };
  if (results.size() != labels.size()) {
    throw std::runtime_error("reexec: failed to download envy " + version + " from " +
                             url + ": unknown error");
  }
  for (size_t i{ 0 }; i < results.size(); ++i) {
    if (auto const *err{ std::get_if<std::string>(&results[i]) }) {
      throw std::runtime_error("reexec: failed to download envy " + version + " from " +
                               urls[i] + ": " + *err);
    }
  }

  // Verify before extract: an unattested archive is never unpacked, so a hostile mirror
  // gets no chance to write paths of its choosing under the temp dir.
  auto const section{ tui::section_create() };

  if (meta.sha256sums) {
    auto const sums_text{ envy_release_load_sums(sums_path, meta.sha256sums, "reexec") };
    envy_release_verify_artifact(archive_path,
                                 archive_name,
                                 sums_text,
                                 "reexec",
                                 tui_actions::byte_progress_bar(section,
                                                                "envy " + version,
                                                                "verifying",
                                                                archive_name));
    tui::debug("attested %s against the pinned %s",
               archive_name.c_str(),
               std::string{ kEnvyReleaseSumsFile }.c_str());
  }

  {
    tui_actions::extract_progress_tracker tracker{ section,
                                                   "envy " + version,
                                                   archive_name };
    extract(archive_path, tmp_dir, { .progress = std::ref(tracker) });
    tracker.finish();
  }

  std::error_code ec;
  std::filesystem::remove(archive_path, ec);

  auto const binary_path{ tmp_dir / platform::exe_name("envy") };
  if (!std::filesystem::exists(binary_path)) {
    throw std::runtime_error("reexec: archive did not contain expected binary: " +
                             binary_path.string());
  }

  make_executable(binary_path);
  remove_quarantine(binary_path);

  throw reexec_request{ binary_path, std::move(drop_options) };
}

}  // namespace envy
