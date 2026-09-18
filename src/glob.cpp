#include "glob.h"

#include "util.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>

namespace envy {

namespace {

// One '/'-delimited component off the front; rest is what follows the separator, empty
// once nothing is left. Canonical paths have no empty components, so "" means exhausted.
struct path_split {
  std::string_view head, rest;
};

path_split split_component(std::string_view path) {
  auto const slash{ path.find('/') };
  return slash == std::string_view::npos
             ? path_split{ path, {} }
             : path_split{ path.substr(0, slash), path.substr(slash + 1) };
}

// Match c against the '[...]' class opening at pat[open]; returns the index past ']'.
// Assumes the class is terminated - glob_normalize_selectors rejects the rest.
std::pair<std::size_t, bool> glob_class_match(std::string_view pat,
                                              std::size_t open,
                                              char c) {
  std::size_t i{ open + 1 };
  bool const negate{ i < pat.size() && (pat[i] == '!' || pat[i] == '^') };
  if (negate) { ++i; }

  bool matched{ false };
  for (bool first{ true }; i < pat.size(); ++i, first = false) {
    if (pat[i] == ']' && !first) {
      ++i;
      break;
    }
    if (i + 2 < pat.size() && pat[i + 1] == '-' && pat[i + 2] != ']') {
      if (c >= pat[i] && c <= pat[i + 2]) { matched = true; }
      i += 2;
      continue;
    }
    if (pat[i] == c) { matched = true; }
  }
  return { i, matched != negate };
}

// Match one path component: '*' any run, '?' one character, '[...]' a class, everything
// else literal. One saved star is enough to be complete, so no recursion and no allocs.
bool glob_component_match(std::string_view pat, std::string_view text) {
  constexpr auto kNone{ std::string_view::npos };
  std::size_t pi{ 0 }, ti{ 0 }, star_pi{ kNone }, star_ti{ 0 };

  while (ti < text.size()) {
    bool advanced{ false };
    if (pi < pat.size()) {
      if (pat[pi] == '*') {
        star_pi = pi++;
        star_ti = ti;
        continue;
      }
      if (pat[pi] == '?') {
        ++pi;
        ++ti;
        continue;
      }
      if (pat[pi] == '[') {
        if (auto const [next, hit]{ glob_class_match(pat, pi, text[ti]) }; hit) {
          pi = next;
          ++ti;
          advanced = true;
        }
      } else if (pat[pi] == text[ti]) {
        ++pi;
        ++ti;
        advanced = true;
      }
    }
    if (advanced) { continue; }
    if (star_pi == kNone) { return false; }
    pi = star_pi + 1;  // let the last '*' eat one more character
    ti = ++star_ti;
  }

  while (pi < pat.size() && pat[pi] == '*') { ++pi; }
  return pi == pat.size();
}

// Why a canonical selector can't be matched unambiguously, or nullopt when it can.
// Rejecting beats guessing: a malformed pattern is a typo, and typos must be loud.
std::optional<std::string_view> selector_problem(std::string_view entry) {
  for (std::string_view rest{ entry }; !rest.empty();) {
    auto const [component, tail]{ split_component(rest) };
    rest = tail;

    if (component.find("**") != std::string_view::npos && component != "**") {
      return "must give '**' a path component of its own";
    }

    for (std::size_t i{ 0 }; i < component.size(); ++i) {
      if (component[i] != '[') { continue; }
      std::size_t scan{ i + 1 };
      if (scan < component.size() && (component[scan] == '!' || component[scan] == '^')) {
        ++scan;
      }
      if (scan < component.size() && component[scan] == ']') { ++scan; }  // literal ']'
      auto const close{ component.find(']', scan) };
      if (close == std::string_view::npos) { return "has an unterminated '[' class"; }
      i = close;
    }
  }
  return std::nullopt;
}

}  // namespace

std::string glob_canonical_path(std::string_view path) {
  std::string collapsed;
  collapsed.reserve(path.size());
  for (char const c : path) {  // '\' is a separator, and "a//b" is "a/b"
    char const sep{ (c == '\\') ? '/' : c };
    if (sep == '/' && !collapsed.empty() && collapsed.back() == '/') { continue; }
    collapsed.push_back(sep);
  }

  std::string_view view{ collapsed };
  while (view.starts_with("./")) { view.remove_prefix(2); }
  while (!view.empty() && view.back() == '/') { view.remove_suffix(1); }
  if (view == ".") { view = {}; }  // "." names the root, which no selector can name
  return std::string{ view };
}

bool glob_is_safe_relative_path(char const *path) {
  if (!path || path[0] == '\0') { return false; }
  if (path[0] == '/' || path[0] == '\\') { return false; }
#ifdef _WIN32
  if (util_ascii_is_alpha(path[0]) && path[1] == ':') { return false; }
#endif
  std::string_view sv{ path };
  // Reject paths containing ".." components
  for (std::size_t pos{ 0 }; pos < sv.size();) {
    auto const sep{ sv.find_first_of("/\\", pos) };
    if (sv.substr(pos, sep == std::string_view::npos ? sep : sep - pos) == "..") {
      return false;
    }
    pos = (sep == std::string_view::npos) ? sv.size() : sep + 1;
  }
  return true;
}

bool glob_match(std::string_view pattern, std::string_view path) {
  std::string_view star_pattern{}, star_path{};
  bool have_star{ false };

  while (true) {
    // Pattern exhausted: whatever is left of the path sits under what already matched,
    // which is how a directory entry pulls in its subtree.
    if (pattern.empty()) { return true; }

    auto const pat{ split_component(pattern) };
    if (pat.head == "**") {  // start by letting it match zero components
      have_star = true;
      star_pattern = pat.rest;
      star_path = path;
      pattern = pat.rest;
      continue;
    }

    if (!path.empty()) {
      if (auto const split{ split_component(path) };
          glob_component_match(pat.head, split.head)) {
        pattern = pat.rest;
        path = split.rest;
        continue;
      }
    }

    if (!have_star || star_path.empty()) { return false; }
    star_path = split_component(star_path).rest;  // '**' eats one more component
    pattern = star_pattern;
    path = star_path;
  }
}

std::vector<std::string> glob_normalize_selectors(
    std::vector<std::string> const &selectors,
    std::string_view context,
    std::string_view what) {
  std::vector<std::string> normalized;
  normalized.reserve(selectors.size());
  for (auto const &raw : selectors) {
    std::string canonical{ glob_canonical_path(raw) };
    if (canonical.empty() || !glob_is_safe_relative_path(canonical.c_str())) {
      throw std::runtime_error(std::string(context) + ": " + std::string(what) +
                               " must be a non-empty relative path without '..': \"" +
                               raw + "\"");
    }
    if (auto const problem{ selector_problem(canonical) }; problem) {
      throw std::runtime_error(std::string(context) + ": " + std::string(what) + " \"" +
                               raw + "\" " + std::string(*problem));
    }
    normalized.push_back(std::move(canonical));
  }
  return normalized;
}

bool glob_any_match(std::vector<std::string> const &selectors, std::string_view path) {
  return std::ranges::any_of(selectors,
                             [path](std::string const &s) { return glob_match(s, path); });
}

bool glob_selectors_match(std::vector<std::string> const &selectors,
                          std::string_view path,
                          std::vector<bool> &matched) {
  matched.resize(selectors.size(), false);
  bool selected{ false };
  for (std::size_t i{ 0 }; i < selectors.size(); ++i) {
    if (glob_match(selectors[i], path)) {
      matched[i] = true;
      selected = true;
    }
  }
  return selected;
}

bool glob_filter::selects(std::string_view path) const {
  if (!include.empty() && !glob_any_match(include, path)) { return false; }
  return exclude.empty() || !glob_any_match(exclude, path);
}

bool glob_filter::selects(std::string_view path,
                          std::vector<bool> &include_matched) const {
  if (!include.empty() && !glob_selectors_match(include, path, include_matched)) {
    return false;
  }
  return exclude.empty() || !glob_any_match(exclude, path);
}

glob_filter glob_parse_filter(std::vector<std::string> const &selectors,
                              std::string_view context,
                              std::string_view what) {
  std::vector<std::string> include, exclude;
  for (auto const &entry : selectors) {
    if (entry == "!") {
      throw std::runtime_error(std::string{ context } + ": " + std::string{ what } +
                               " \"!\" excludes nothing; give it a pattern");
    }
    bool const negated{ entry.starts_with('!') };
    (negated ? exclude : include).push_back(negated ? entry.substr(1) : entry);
  }
  return { .include = glob_normalize_selectors(include, context, what),
           .exclude = glob_normalize_selectors(exclude, context, what) };
}

std::vector<std::string> glob_unmatched_selectors(
    std::vector<std::string> const &selectors,
    std::vector<bool> const &matched) {
  std::vector<std::string> unmatched;
  for (std::size_t i{ 0 }; i < selectors.size(); ++i) {
    if (i >= matched.size() || !matched[i]) { unmatched.push_back(selectors[i]); }
  }
  return unmatched;
}

}  // namespace envy
