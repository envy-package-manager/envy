#pragma once

#include "manifest.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace envy {

// The requested envy, on disk and ready to run. Thrown rather than returned: a caller that
// forgot to propagate a return value would carry on as the wrong version, and only main
// holds the argv this has to be exec'd with anyway.
struct reexec_request {
  std::filesystem::path binary;
  // Parent-side flags the child must not see: an option consumed to choose *which* envy
  // runs means nothing to that envy, and a release predating it rejects it outright.
  std::vector<std::string> drop_options;
};

// Called by manifest-aware commands after discovering metadata.
// If version mismatch: downloads the correct envy, then throws reexec_request.
// Returns normally if: no @envy version, version matches, dev build (0.0.0),
// ENVY_REEXEC set, or ENVY_NO_REEXEC set.
// `cache_root` is the already-resolved root, used to look for the requested version
// already in cache before downloading it.
void reexec_if_needed(envy_meta const &meta,
                      std::filesystem::path const &cache_root,
                      std::filesystem::path const &manifest_dir,
                      std::vector<std::string> drop_options = {});

// Become the requested envy, with the loop guard set. Never returns where exec replaces
// the process; elsewhere, the child's exit code.
int reexec_exec(reexec_request const &request, char **argv);

// What the child is handed: `argv` minus every option the request drops, null-terminated,
// sharing argv's strings. All of reexec_exec except the exec itself.
std::vector<char *> reexec_child_argv(reexec_request const &request, char **argv);

// `argv` minus '<option> <value>' and '<option>=<value>', null-terminated, sharing argv's
// strings. The filtering reexec_exec applies, exposed for its own sake.
std::vector<char *> reexec_argv_without(char **argv, std::string_view option);

// The version this build reports, ENVY_TEST_SELF_VERSION included. What the scripts a
// deploy stamps are stamped from, which has to match the version the manifest pins.
std::string_view reexec_self_version();

enum class reexec_decision { PROCEED, REEXEC };

reexec_decision reexec_should(std::string_view self_version,
                              std::optional<std::string> const &requested_version,
                              bool reexec_env_set,
                              bool no_reexec_env_set);

}  // namespace envy
