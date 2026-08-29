#pragma once

#include "cache.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace envy::self_deploy {

// Create/open cache, self-deploy running binary + types, update latest, and -- for a
// non-LOCAL root only -- ensure the shell hooks.
//
// Takes an already-resolved root: the cache root is resolved once per process (see
// cmd_startup_load) so a concurrent `envy cache --local` cannot land between two
// resolutions and split one run across two trees. `mode` travels beside it rather than as
// a whole cache_root_resolution because `envy cache --local/--shared` deploys into a mode
// it is about to establish, and there is no tier to name for a marker not yet written.
std::unique_ptr<cache> ensure(std::filesystem::path const &root, cache_mode mode);

}  // namespace envy::self_deploy
