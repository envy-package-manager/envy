#pragma once

#include <filesystem>

namespace envy::shell_hooks {

// Write/update every shell hook in cache_root/shell/: one whose bytes already match this
// binary's copy is left alone, anything else is replaced. Returns the number written.
int ensure(std::filesystem::path const &cache_root);

}  // namespace envy::shell_hooks
