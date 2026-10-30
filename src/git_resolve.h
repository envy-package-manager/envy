#pragma once

#include <string>
#include <string_view>
#include <vector>

struct git_remote;

namespace envy {

// One advertised ref from a remote's ref advertisement. `oid` is the full
// lowercase hex object id the ref points at. Annotated tags additionally
// advertise a peeled entry whose `name` ends in "^{}" and whose `oid` is the
// commit the tag ultimately points at.
struct git_ref_entry {
  std::string name;
  std::string oid;
};

// True if `s` is a full git object id: 40 hex chars (SHA-1) or 64 (SHA-256).
bool git_ref_is_full_sha(std::string_view s);

// Resolve `ref` against a remote's advertised ref list to a full commit sha.
//
// Matching, in order:
//   1. Exact: an advertised name equal to `ref`. Annotated tags are peeled --
//      when an entry named `ref .. "^{}"` exists, its oid (the commit the tag
//      points at) is returned in preference to the tag-object oid.
//   2. Suffix: if nothing matches exactly, advertised names ending in
//      "/" .. `ref` (e.g. ref "v1.2.3" matching "refs/tags/v1.2.3"). Resolves
//      only when every such match points at the same oid.
//
// Throws std::runtime_error when `ref` is empty, matches nothing, is advertised
// with conflicting oids, or when a suffix match is ambiguous across oids.
std::string git_resolve_ref(std::vector<git_ref_entry> const &refs, std::string_view ref);

// A connected remote's advertisement, copied out of the connection it borrows from.
std::vector<git_ref_entry> git_remote_refs(git_remote *remote);

// Resolve `ref` (tag/branch/sha) in remote `repo` to a full commit sha.
//
// A full 40/64-hex sha is returned lowercased without any network access.
// Otherwise the remote's ref advertisement is fetched (libgit2 ls-remote; no
// clone, no `git` binary) and matched via git_resolve_ref. HTTPS remotes
// require the system CA bundle be configured.
//
// Throws std::runtime_error when `repo` or `ref` is empty, the remote cannot
// be contacted, or the ref does not resolve (see git_resolve_ref).
std::string git_resolve_remote(std::string const &repo, std::string const &ref);

}  // namespace envy
