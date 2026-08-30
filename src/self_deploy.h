#pragma once

#include "cache.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace envy::self_deploy {

// Self-deploy the binary + types, update latest, and -- for a non-LOCAL root -- the shell
// hooks. The root is resolved once per process, so no run is split across two trees.
std::unique_ptr<cache> ensure(std::filesystem::path const &root, cache_mode mode);

}  // namespace envy::self_deploy
