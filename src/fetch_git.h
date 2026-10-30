#pragma once

#include "fetch.h"
#include "fetch_error.h"
#include "git_resolve.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct git_indexer_progress;

namespace envy {

// How one repository's commit is fetched. Chosen from the remote's advertisement, so a
// commit any ref points at never depends on the server taking unadvertised wants.
enum class git_fetch_strategy {
  LOCAL,  // local transport: the commit by id at full depth; libgit2 cannot go shallow
  TIP,    // an advertised ref points at the commit: that ref, depth 1, on any server
  OID,    // the commit by id at depth 1, for servers that take unadvertised wants
  FULL,   // every branch and tag at full depth; what a server refusing OID leaves
};

char const *git_fetch_strategy_name(git_fetch_strategy strategy);

struct git_fetch_plan {
  git_fetch_strategy strategy;
  std::vector<std::string> refspecs;
  int depth;  // 0 = full history
};

// Plans to try in order for full lowercase commit `sha`, each shallow one followed by a
// full-depth fallback: only the server knows whether it takes an unadvertised want.
std::vector<git_fetch_plan> git_fetch_plans(std::string_view sha,
                                            std::vector<git_ref_entry> const &refs,
                                            bool local);

// The first of `plans` that yields commit `sha`. `download` throws PROTOCOL for a plan the
// server refuses, and the next plan runs; any other failure ends the attempt.
git_fetch_plan const &git_fetch_first(std::vector<git_fetch_plan> const &plans,
                                      std::string_view url,
                                      std::string_view sha,
                                      function_ref<void(git_fetch_plan const &)> download,
                                      function_ref<bool()> has_commit);

// Two submodule names whose gitdirs, `.git/modules/<name>`, would nest one in the other.
std::optional<std::pair<std::string, std::string>> git_submodule_name_clash(
    std::vector<std::string> const &names);

// `ref` as a full lowercase commit id: a full sha as written, else an advertised name.
// Throws PROTOCOL for anything else -- a short sha or `HEAD~1` cannot be fetched shallow.
std::string git_fetch_target(std::string_view ref, std::vector<git_ref_entry> const &refs);

// A path or file:// URL: what libgit2 serves through its local transport.
bool git_url_is_local(std::string_view url);

// Resolve a .gitmodules url against its superproject's, as git does: "./" stays put,
// each "../" drops one trailing component. Any other url is already absolute.
std::string git_submodule_url(std::string_view super, std::string_view url);

// libgit2's error class and message as a transport verdict. A server's ERR packet, a 4xx,
// or a reply libgit2 cannot parse is an answer, not a dropped connection: final.
fetch_error_kind git_error_kind(int klass, std::string_view message);

// Every repository's progress, rolled into the request's one row. Until a submodule is
// registered that row is the root's own fetch, like any clone; after, it is the tree --
// X/Y submodules over the whole tree's bytes, each in-flight repository beneath. libgit2
// and retry waits call in from every worker, so the row's callback runs under one lock.
class git_progress : unmovable {
 public:
  struct repo {
    git_progress *sum;
    std::string path;  // empty for the root
    fetch_git_progress last{};
    std::optional<fetch_retry_progress> retry;  // set while it waits out a backoff
    bool done{ false };
  };

  explicit git_progress(fetch_progress_cb_t const &row) : row_{ row } {}

  repo *track(std::string path);  // a repository about to fetch
  void finished(repo *r);         // fetched and checked out
  void discovered(std::size_t count);
  fetch_progress_cb_t waits(repo *r);  // fetch_retrying's report: its backoff waits
  bool aborted() const { return aborted_; }

  static int on_transfer(git_indexer_progress const *stats, void *payload);

 private:
  bool publish();  // mutex_ held
  fetch_progress_t snapshot() const;

  fetch_progress_cb_t const &row_;
  std::mutex mutex_;
  std::deque<repo> repos_;  // stable addresses: each is a libgit2 callback payload
  std::uint32_t done_{ 0 };
  std::uint32_t total_{ 0 };
  std::atomic<bool> aborted_{ false };
};

// Fetch `req.ref` of `req.source` into the empty directory `dest`, then every submodule
// req.submodules selects, recursively, in git's layout. Each repository retries alone.
void fetch_git(fetch_request_git const &req,
               std::filesystem::path const &dest,
               std::string const &trace_spec);

}  // namespace envy
