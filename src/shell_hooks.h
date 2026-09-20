#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace envy::shell_hooks {

// The one line an update announces itself with, naming every shell whose hook changed.
// Empty when none did: four "restart your shell" lines for one restart is noise.
std::string updated_message(std::vector<char const *> const &shells);

// Write/update every shell hook in cache_root/shell/: one whose bytes already match this
// binary's copy is left alone, anything else is replaced. Returns the number written.
int ensure(std::filesystem::path const &cache_root);

}  // namespace envy::shell_hooks
