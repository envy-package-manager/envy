#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace envy {

class engine;
struct manifest;
struct pkg;

// <project_root>/.envy/state. envy owns the entry names; each package's tool fills one.
std::filesystem::path pkg_state_root(std::filesystem::path const &project_root);

// An identity sans revision ("envy.go@r0" -> "envy.go"): every revision and option set of
// a spec shares one dir, so a spec rev or version bump keeps the tool's caches.
std::string_view pkg_state_key(std::string_view identity);

// p's mutable state for ENV and command functions. root is the project in a shell, a
// scratch dir in a phase.
std::filesystem::path pkg_state_dir(pkg const *p, std::filesystem::path const &root);

struct pkg_state_entry {
  std::string name;  // UTF-8, as queries and output spell it
  std::filesystem::path path;
  std::filesystem::file_type type;  // lstat's: a link or junction reads as itself
};

// The entries under pkg_state_root(project_root), name-sorted; a missing root has none.
std::vector<pkg_state_entry> pkg_state_entries(std::filesystem::path const &project_root,
                                               std::error_code &ec);
std::vector<pkg_state_entry> pkg_state_entries(std::filesystem::path const &project_root);

// Names of the `entries` unused by `eng`'s packages. A case variant counts as used where
// the filesystem resolves both spellings to one directory.
std::vector<std::string> pkg_state_unused(engine const &eng,
                                          std::filesystem::path const &project_root,
                                          std::vector<pkg_state_entry> const &entries);

// One INFO line naming m's unused state entries, which stay in place: a branch switch
// makes live caches look unused.
void pkg_state_report_unused(engine const &eng, manifest const &m);

}  // namespace envy
