#pragma once

#include "fetch.h"
#include "glob.h"

#include "sol/forward.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace envy {

class engine;
struct pkg;

void run_fetch_phase(pkg *p, engine &eng);

// One FETCH or envy.fetch table entry. Any key neither reads is an error: a misspelled
// `submodules` would otherwise fetch the tree without them, silently.
struct fetch_entry {
  std::string source;
  std::optional<std::string> sha256, ref, post_data, dest;
  std::optional<glob_filter> submodules;
};

fetch_entry fetch_entry_parse(sol::table const &tbl, std::string_view context);

fetch_request url_to_fetch_request(
    fetch_entry const &entry,
    std::filesystem::path const &dest,
    std::string const &context,
    std::optional<std::filesystem::path> const &file_root = {});

}  // namespace envy
