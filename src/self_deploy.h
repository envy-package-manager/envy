#pragma once

#include "cache.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace envy::self_deploy {

// Create/open cache, self-deploy running binary + types, update latest, ensure hooks.
// Takes an already-resolved root: the cache root is resolved once per process (see
// cmd_load_manifest_and_cache) so a concurrent `envy cache --local` cannot land between
// two resolutions and split one run across two trees.
std::unique_ptr<cache> ensure(std::filesystem::path const &root);

}  // namespace envy::self_deploy
