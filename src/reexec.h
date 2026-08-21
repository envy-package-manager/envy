#pragma once

#include "manifest.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace envy {

void reexec_init(char **argv);

// Drop an option, and its value, from the argv the re-exec'd child receives. A flag the
// parent consumed to choose *which* envy runs means nothing to that envy, and a release
// predating the flag rejects the unknown option outright.
void reexec_drop_option(std::string_view option);

// The filtering reexec_drop_option applies: `argv` minus '<option> <value>' and
// '<option>=<value>', null-terminated, sharing argv's strings.
std::vector<char *> reexec_argv_without(char **argv, std::string_view option);

// Called by manifest-aware commands after discovering metadata.
// If version mismatch: downloads correct envy to cache, re-execs (never returns).
// Returns normally if: no @envy version, version matches, dev build (0.0.0),
// ENVY_REEXEC set, or ENVY_NO_REEXEC set.
// `manifest_dir` anchors a relative '@envy cache-*' directive.
void reexec_if_needed(envy_meta const &meta,
                      std::optional<std::filesystem::path> const &cli_cache_root,
                      std::filesystem::path const &manifest_dir);

enum class reexec_decision { PROCEED, REEXEC };

reexec_decision reexec_should(std::string_view self_version,
                              std::optional<std::string> const &requested_version,
                              bool reexec_env_set,
                              bool no_reexec_env_set);

}  // namespace envy
