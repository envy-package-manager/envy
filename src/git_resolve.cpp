#include "git_resolve.h"

#include "libgit2_util.h"
#include "trace.h"
#include "util.h"

#include <git2.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace envy {
namespace {

constexpr std::string_view kPeelSuffix{ "^{}" };

bool ends_with(std::string const &s, std::string_view suffix) {
  return s.size() >= suffix.size() &&
         std::string_view{ s }.substr(s.size() - suffix.size()) == suffix;
}

// Build a std::runtime_error, appending libgit2's thread-local last-error text.
std::runtime_error git_last_error(std::string msg) {
  if (git_error const *e{ git_error_last() }; e && e->message) {
    msg += ": ";
    msg += e->message;
  }
  return std::runtime_error(msg);
}

}  // namespace

bool git_ref_is_full_sha(std::string_view s) {
  if (s.size() != 40 && s.size() != 64) { return false; }
  for (char const c : s) {
    bool const is_hex{ (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F') };
    if (!is_hex) { return false; }
  }
  return true;
}

std::string git_resolve_ref(std::vector<git_ref_entry> const &refs, std::string_view ref) {
  if (ref.empty()) { throw_runtime_error({ "git-resolve: ref must be non-empty" }); }

  // Collapse the advertisement into one oid per base name. Plain entries carry
  // the ref's own oid; peeled ("<name>^{}") entries carry the underlying commit
  // and take precedence. Transparent comparator enables string_view lookups.
  std::map<std::string, std::string, std::less<>> plain;
  std::map<std::string, std::string, std::less<>> peeled;

  for (auto const &e : refs) {
    if (ends_with(e.name, kPeelSuffix)) {
      std::string base{ e.name.substr(0, e.name.size() - kPeelSuffix.size()) };
      auto const [it, inserted]{ peeled.try_emplace(std::move(base), e.oid) };
      if (!inserted && it->second != e.oid) {
        throw_runtime_error({ "git-resolve: ref '",
                              it->first,
                              "' advertised with conflicting peeled oids" });
      }
    } else {
      auto const [it, inserted]{ plain.try_emplace(e.name, e.oid) };
      if (!inserted && it->second != e.oid) {
        throw_runtime_error(
            { "git-resolve: ref '", e.name, "' advertised with conflicting oids" });
      }
    }
  }

  // Canonical resolved oid per base name: peeled wins over plain.
  std::map<std::string, std::string, std::less<>> const canon{ [&] {
    std::map<std::string, std::string, std::less<>> m;
    for (auto const &[name, oid] : plain) { m.emplace(name, oid); }
    for (auto const &[base, oid] : peeled) { m[base] = oid; }
    return m;
  }() };

  // 1. Exact name.
  if (auto const it{ canon.find(ref) }; it != canon.end()) { return it->second; }

  // 2. Suffix "/" + ref (a trailing path segment run of an advertised name).
  std::string needle{ "/" };
  needle.append(ref);
  std::vector<std::pair<std::string, std::string>> matches;
  for (auto const &[name, oid] : canon) {
    if (ends_with(name, needle)) { matches.emplace_back(name, oid); }
  }

  if (matches.empty()) { throw_runtime_error({ "git-resolve: ref not found: ", ref }); }

  bool const all_same{ std::all_of(matches.begin(), matches.end(), [&](auto const &m) {
    return m.second == matches.front().second;
  }) };
  if (!all_same) {
    std::ostringstream msg;
    msg << "git-resolve: ref '" << ref << "' is ambiguous; matches:";
    for (auto const &[name, oid] : matches) { msg << "\n  " << name << " -> " << oid; }
    throw_runtime_error({ msg.str() });
  }

  return matches.front().second;
}

std::string git_resolve_remote(std::string const &repo, std::string const &ref) {
  if (repo.empty()) { throw_runtime_error({ "git-resolve: repo must be non-empty" }); }
  if (ref.empty()) { throw_runtime_error({ "git-resolve: ref must be non-empty" }); }

  // A full object id needs no lookup -- return it (no network). Normalize to
  // lowercase so it matches the lowercase oids the ref-advertisement path emits.
  if (git_ref_is_full_sha(ref)) {
    std::string lower{ ref };
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    ENVY_TRACE(git_resolve, "", .url = repo, .ref = ref, .sha = lower, .method = "sha");
    return lower;
  }

  // HTTPS requires CA certificates be configured (matches fetch_git_repo).
  if (repo.starts_with("https://")) { libgit2_require_ssl_certs(); }

  std::unique_ptr<git_remote, decltype(&git_remote_free)> const remote{ [&] {
    git_remote *raw{ nullptr };
    if (git_remote_create_detached(&raw, repo.c_str())) {
      throw git_last_error("git-resolve: invalid remote '" + repo + "'");
    }
    return std::unique_ptr<git_remote, decltype(&git_remote_free)>{ raw, git_remote_free };
  }() };

  git_remote_callbacks callbacks;
  git_remote_init_callbacks(&callbacks, GIT_REMOTE_CALLBACKS_VERSION);

  if (git_remote_connect(remote.get(),
                         GIT_DIRECTION_FETCH,
                         &callbacks,
                         nullptr,
                         nullptr)) {
    throw git_last_error("git-resolve: cannot connect to '" + repo + "'");
  }

  // git_remote_ls borrows from the connection; disconnect after copying out.
  struct disconnect_guard {
    git_remote *r;
    ~disconnect_guard() { git_remote_disconnect(r); }
  } const guard{ remote.get() };

  git_remote_head const **heads{ nullptr };
  size_t count{ 0 };
  if (git_remote_ls(&heads, &count, remote.get())) {
    throw git_last_error("git-resolve: cannot list refs of '" + repo + "'");
  }

  std::vector<git_ref_entry> entries;
  entries.reserve(count);
  char oid_hex[GIT_OID_MAX_HEXSIZE + 1];
  for (size_t i{ 0 }; i < count; ++i) {
    git_oid_tostr(oid_hex, sizeof(oid_hex), &heads[i]->oid);
    entries.push_back({ heads[i]->name ? heads[i]->name : "", std::string{ oid_hex } });
  }

  auto sha{ git_resolve_ref(entries, ref) };
  ENVY_TRACE(git_resolve, "", .url = repo, .ref = ref, .sha = sha, .method = "ls-remote");
  return sha;
}

}  // namespace envy
