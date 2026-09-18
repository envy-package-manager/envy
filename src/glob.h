#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace envy {

// Glob matching over '/'-joined relative paths. Shared by archive extraction's `only`
// and a spec's VENDOR list, which are one selector language.

// Canonicalize for matching: '\' to '/', repeated '/' collapsed, leading "./" and
// trailing '/' dropped. "." becomes empty; no pattern can name the root.
std::string glob_canonical_path(std::string_view path);

// Non-empty, relative, no ".." component, no drive letter.
bool glob_is_safe_relative_path(char const *path);

// '*' and '?' stay inside one component, '**' spans them, '[a-z]'/'[!a-z]' are classes.
// A pattern naming a directory matches everything under it.
bool glob_match(std::string_view pattern, std::string_view path);

// Canonicalize and reject the unusable: empty, absolute, "..", unterminated '[', '**'
// sharing a component. Errors read "<context>: <what> ...".
std::vector<std::string> glob_normalize_selectors(
    std::vector<std::string> const &selectors,
    std::string_view context,
    std::string_view what);

// Stops at the first hit and allocates nothing, for callers that only need the answer.
bool glob_any_match(std::vector<std::string> const &selectors, std::string_view path);

// Also flags each selector that matched, so a caller can report the ones that did not.
bool glob_selectors_match(std::vector<std::string> const &selectors,
                          std::string_view path,
                          std::vector<bool> &matched);

// Take an entry when `include` is empty or one include matches, and no exclude does.
struct glob_filter {
  std::vector<std::string> include, exclude;

  bool empty() const { return include.empty() && exclude.empty(); }

  bool selects(std::string_view path) const;

  // Flags matching includes. Excludes are not tracked: an exclude naming nothing is a
  // set that lacked it, while an include naming nothing is a typo.
  bool selects(std::string_view path, std::vector<bool> &include_matched) const;
};

// A leading '!' excludes. Both halves go through glob_normalize_selectors. A bare "!"
// excludes nothing and throws.
glob_filter glob_parse_filter(std::vector<std::string> const &selectors,
                              std::string_view context,
                              std::string_view what);

// Selectors never flagged in `matched`, in declaration order.
std::vector<std::string> glob_unmatched_selectors(
    std::vector<std::string> const &selectors,
    std::vector<bool> const &matched);

}  // namespace envy
