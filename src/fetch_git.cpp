#include "fetch_git.h"

#include "glob.h"
#include "libgit2_util.h"
#include "trace.h"
#include "tui.h"
#include "uri.h"
#include "util.h"

#include "git2.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace envy {
namespace {

constexpr std::size_t kMaxGitJobs{ 8 };  // submodule fetches in flight, tree-wide

using repo_ptr = std::unique_ptr<git_repository, decltype(&git_repository_free)>;
using remote_ptr = std::unique_ptr<git_remote, decltype(&git_remote_free)>;
using object_ptr = std::unique_ptr<git_object, decltype(&git_object_free)>;
using config_ptr = std::unique_ptr<git_config, decltype(&git_config_free)>;

// A fetch_error from message pieces, joined out of line like throw_runtime_error's.
[[noreturn]] void throw_fetch_error(fetch_error_kind kind,
                                    std::initializer_list<err_part> parts) {
  std::string what;
  for (auto const &part : parts) { part.append_to(what); }
  throw fetch_error(kind, what);
}

// `context` reads as the head of a sentence that libgit2's own message completes.
[[noreturn]] void throw_git_error(std::initializer_list<err_part> context) {
  git_error const *err{ git_error_last() };
  std::string_view const message{ err && err->message ? err->message : "" };
  std::string what;
  for (auto const &part : context) { part.append_to(what); }
  throw fetch_error(git_error_kind(err ? err->klass : GIT_ERROR_NONE, message),
                    what.append(message));
}

std::string lowercase(std::string_view s) {
  std::string out{ s };
  std::ranges::transform(out, out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

// What a gitdir name or a checkout path must be to stay inside the tree it names.
bool safe_tree_path(std::string const &p) {
  if (!glob_is_safe_relative_path(p.c_str())) { return false; }
  return std::ranges::none_of(std::filesystem::path{ p }, [](auto const &part) {
    return lowercase(part.generic_string()) == ".git";
  });
}

// A symlink the superproject checked out would carry the submodule's checkout -- and
// its writes -- out of the tree (CVE-2024-32002); a file there is no place for one.
void require_real_dirs(std::filesystem::path const &workdir, std::string const &rel) {
  std::filesystem::path p{ workdir };
  for (auto const &part : std::filesystem::path{ rel }) {
    p /= part;
    std::error_code ec;
    auto const st{ std::filesystem::symlink_status(p, ec) };
    if (ec || !std::filesystem::exists(st)) { return; }
    if (!std::filesystem::is_directory(st)) {
      throw_runtime_error(
          { "fetch_git: submodule path '", rel, "' crosses a non-directory at ", p });
    }
  }
}

config_ptr repo_config(git_repository *repo) {
  git_config *raw{ nullptr };
  if (git_repository_config(&raw, repo)) {
    throw_git_error({ "fetch_git: cannot open repository config: " });
  }
  return config_ptr{ raw, git_config_free };
}

bool has_commit(git_repository *repo, std::string const &sha) {
  git_oid oid;
  git_object *raw{ nullptr };
  if (git_oid_fromstr(&oid, sha.c_str()) ||
      git_object_lookup(&raw, repo, &oid, GIT_OBJECT_COMMIT)) {
    return false;
  }
  git_object_free(raw);
  return true;
}

repo_ptr init_repo(std::filesystem::path const &gitdir,
                   std::filesystem::path const *workdir,
                   std::string const &url) {
  std::string const workdir_str{ workdir ? workdir->string() : std::string{} };
  git_repository_init_options opts{ [&] {
    git_repository_init_options o;
    git_repository_init_options_init(&o, GIT_REPOSITORY_INIT_OPTIONS_VERSION);
    o.flags = GIT_REPOSITORY_INIT_MKPATH | GIT_REPOSITORY_INIT_NO_REINIT;
    if (workdir) {  // a submodule: gitdir under .git/modules, a relative gitfile in the
                    // tree
      o.workdir_path = workdir_str.c_str();
      o.flags |= GIT_REPOSITORY_INIT_NO_DOTGIT_DIR | GIT_REPOSITORY_INIT_RELATIVE_GITLINK;
    }
    o.origin_url = url.c_str();
    return o;
  }() };

  git_repository *raw{ nullptr };
  if (git_repository_init_ext(&raw, gitdir.string().c_str(), &opts)) {
    throw_git_error({ "fetch_git: cannot create ", gitdir, ": " });
  }
  repo_ptr repo{ raw, git_repository_free };

#ifdef _WIN32
  // Submodule trees nest deep under a cache root; MAX_PATH is no limit git keeps either.
  if (git_config_set_bool(repo_config(repo.get()).get(), "core.longpaths", 1)) {
    throw_git_error({ "fetch_git: cannot configure ", gitdir, ": " });
  }
#endif
  return repo;
}

struct submodule_entry {
  std::string name, path, url, sha;  // url as .gitmodules writes it; empty if absent
  bool update_none;
};

// Every submodule the checked-out commit records a gitlink for; a .gitmodules stanza
// without one has nothing to check out.
std::vector<submodule_entry> list_submodules(git_repository *repo) {
  std::vector<submodule_entry> out;
  if (git_submodule_foreach(
          repo,
          [](git_submodule *sm, char const *name, void *payload) -> int {
            git_oid const *head{ git_submodule_head_id(sm) };
            if (!head) { return 0; }
            char hex[GIT_OID_MAX_HEXSIZE + 1];
            git_oid_tostr(hex, sizeof(hex), head);
            char const *url{ git_submodule_url(sm) };
            static_cast<std::vector<submodule_entry> *>(payload)->push_back(
                { .name = name,
                  .path = git_submodule_path(sm),
                  .url = url ? url : "",
                  .sha = hex,
                  .update_none =
                      git_submodule_update_strategy(sm) == GIT_SUBMODULE_UPDATE_NONE });
            return 0;
          },
          &out)) {
    throw_git_error({ "fetch_git: cannot read submodules: " });
  }
  util_sort(out, [](auto const &a, auto const &b) { return a.path < b.path; });
  return out;
}

struct git_job {
  std::string url;   // resolved
  std::string ref;   // as written for the root; the recorded gitlink below it
  std::string path;  // '/'-joined from the root; empty for the root itself
  repo_ptr repo;
};

// One request's whole tree: the root on the calling thread, then its submodules on a
// bounded pool. A repository registers its children -- config, gitdir, gitfile -- before
// any is queued, so no two workers ever write one repository.
class git_tree : unmovable {
 public:
  git_tree(fetch_request_git const &req, std::string const &trace_spec)
      : filter_{ req.submodules }, trace_spec_{ trace_spec }, progress_{ req.progress } {}

  void run(std::filesystem::path const &dest, std::string url, std::string ref) {
    auto repo{ init_repo(dest, nullptr, url) };
    process(git_job{ .url = std::move(url),
                     .ref = std::move(ref),
                     .path = {},
                     .repo = std::move(repo) });
    if (!queue_.empty()) { drain(); }

    if (!filter_) { return; }
    std::string unmatched;
    for (auto const &s : glob_unmatched_selectors(filter_->include, matched_)) {
      unmatched += (unmatched.empty() ? "" : ", ") + s;
    }
    if (!unmatched.empty()) {
      throw_runtime_error(
          { "fetch_git: submodule selector matched nothing: ", unmatched });
    }
  }

 private:
  void drain() {
    std::string const log_ctx{ tui::log_ctx() };
    std::vector<std::thread> workers;
    workers.reserve(kMaxGitJobs);
    for (std::size_t i{ 0 }; i < kMaxGitJobs; ++i) {
      workers.emplace_back([&] {
        tui::log_ctx_scope const scope{ log_ctx };
        work();
      });
    }
    for (auto &w : workers) { w.join(); }
    if (error_) { std::rethrow_exception(error_); }
  }

  void work() {
    for (;;) {
      std::optional<git_job> job;
      {
        std::unique_lock lock{ mutex_ };
        cv_.wait(lock, [&] { return error_ || !queue_.empty() || busy_ == 0; });
        if (error_ || queue_.empty()) { return; }
        job.emplace(std::move(queue_.front()));
        queue_.pop_front();
        ++busy_;
      }

      std::exception_ptr failure;
      try {
        process(std::move(*job));
      } catch (...) { failure = std::current_exception(); }

      {
        std::lock_guard const lock{ mutex_ };
        if (failure && !error_) { error_ = failure; }
        --busy_;
      }
      cv_.notify_all();
    }
  }

  void process(git_job job) {
    fetch_repo(job);
    if (!filter_) { return; }

    auto children{ register_children(job, list_submodules(job.repo.get())) };
    if (children.empty()) { return; }
    progress_.discovered(children.size());
    {
      std::lock_guard const lock{ mutex_ };
      for (auto &c : children) { queue_.push_back(std::move(c)); }
    }
    cv_.notify_all();
  }

  // Connect, pick a plan from the advertisement, download, and check the commit out
  // detached. The retry wraps the network half only: a repository is created once.
  void fetch_repo(git_job const &job) {
    if (uri_is_https_scheme(job.url)) { libgit2_require_ssl_certs(); }

    // One remote per connection: libgit2 cannot reconnect one whose fetch failed.
    auto const origin{ [&] {
      git_remote *raw{ nullptr };
      if (git_remote_lookup(&raw, job.repo.get(), "origin")) {
        throw_git_error({ "fetch_git: no origin for ", job.url, ": " });
      }
      return remote_ptr{ raw, git_remote_free };
    } };

    git_progress::repo *const prog{ progress_.track(job.path) };
    git_remote_callbacks const callbacks{ [&] {
      git_remote_callbacks c;
      git_remote_init_callbacks(&c, GIT_REMOTE_CALLBACKS_VERSION);
      c.transfer_progress = git_progress::on_transfer;
      c.payload = prog;
      return c;
    }() };

    // Only an abort can stop the row's callback, so its flag decides what a failure was.
    auto const fail{ [&](char const *what) {
      if (progress_.aborted()) {
        throw_fetch_error(fetch_error_kind::ABORTED, { "fetch_git: aborted: ", job.url });
      }
      throw_git_error({ "fetch_git: ", what, " ", job.url, " failed: " });
    } };

    std::string sha;
    std::vector<git_fetch_plan> plans;
    git_fetch_plan const *fetched{ nullptr };
    fetch_retrying(
        [&] {
          auto const refs{ [&] {
            remote_ptr const remote{ origin() };
            if (git_remote_connect(remote.get(),
                                   GIT_DIRECTION_FETCH,
                                   &callbacks,
                                   nullptr,
                                   nullptr)) {
              fail("connect to");
            }
            auto r{ git_remote_refs(remote.get()) };
            git_remote_disconnect(remote.get());
            return r;
          }() };
          sha = git_fetch_target(job.ref, refs);
          plans = git_fetch_plans(sha, refs, git_url_is_local(job.url));

          fetched = &git_fetch_first(
              plans,
              job.url,
              sha,
              [&](git_fetch_plan const &plan) {
                download(origin().get(), plan, callbacks, fail);
              },
              [&] { return has_commit(job.repo.get(), sha); });
        },
        progress_.waits(prog),
        trace_spec_,
        job.url);

    checkout(job, sha, prog);
    progress_.finished(prog);

    ENVY_TRACE(git_fetch,
               trace_spec_,
               .url = job.url,
               .path = job.path,
               .sha = sha,
               .strategy = git_fetch_strategy_name(fetched->strategy),
               .depth = fetched->depth);
    tui::debug("fetch: git %s @ %s (%s, depth %d)",
               job.path.empty() ? job.url.c_str() : job.path.c_str(),
               sha.c_str(),
               git_fetch_strategy_name(fetched->strategy),
               fetched->depth);
  }

  template <typename Fail>
  static void download(git_remote *remote,
                       git_fetch_plan const &plan,
                       git_remote_callbacks const &callbacks,
                       Fail const &fail) {
    git_fetch_options const opts{ [&] {
      git_fetch_options o;
      git_fetch_options_init(&o, GIT_FETCH_OPTIONS_VERSION);
      o.callbacks = callbacks;
      o.depth = plan.depth;
      o.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_NONE;  // the plan's refspecs name any
      return o;
    }() };

    std::vector<char *> specs;
    for (auto const &s : plan.refspecs) { specs.push_back(const_cast<char *>(s.c_str())); }
    git_strarray const arr{ specs.data(), specs.size() };
    if (git_remote_fetch(remote, &arr, &opts, nullptr)) { fail("fetch from"); }
  }

  static void checkout(git_job const &job,
                       std::string const &sha,
                       git_progress::repo *prog) {
    git_oid oid;
    git_object *raw{ nullptr };
    if (git_oid_fromstr(&oid, sha.c_str()) ||
        git_object_lookup(&raw, job.repo.get(), &oid, GIT_OBJECT_COMMIT)) {
      throw_fetch_error(fetch_error_kind::PROTOCOL,
                        { "fetch_git: ", job.url, " has no commit ", sha });
    }
    object_ptr const commit{ raw, git_object_free };

    git_checkout_options const opts{ [&] {
      git_checkout_options o;
      git_checkout_options_init(&o, GIT_CHECKOUT_OPTIONS_VERSION);
      o.checkout_strategy = GIT_CHECKOUT_FORCE;
      o.progress_cb = git_progress::on_checkout;
      o.progress_payload = prog;
      return o;
    }() };
    if (git_checkout_tree(job.repo.get(), commit.get(), &opts)) {
      throw_git_error({ "fetch_git: checkout of ", sha, " failed: " });
    }
    if (git_repository_set_head_detached(job.repo.get(), &oid)) {
      throw_git_error({ "fetch_git: cannot detach HEAD at ", sha, ": " });
    }
  }

  // Each selected submodule of a checked-out repository, registered the way
  // `git submodule update --init` leaves it and ready to fetch.
  std::vector<git_job> register_children(git_job const &job,
                                         std::vector<submodule_entry> const &subs) {
    std::filesystem::path const workdir{ git_repository_workdir(job.repo.get()) };
    std::filesystem::path const gitdir{ git_repository_path(job.repo.get()) };

    std::vector<submodule_entry const *> picked;
    for (auto const &sm : subs) {
      std::string const full{ job.path.empty() ? sm.path : job.path + "/" + sm.path };
      // Selected, or on the way to something an include may select: only fetching a
      // submodule says what it holds.
      bool const wanted{ [&] {
        std::lock_guard const lock{ mutex_ };
        return filter_->selects(full, matched_) || filter_->reaches_below(full);
      }() };
      if (!wanted || sm.update_none) {
        tui::debug("fetch: submodule %s skipped (%s)",
                   full.c_str(),
                   wanted ? "update = none" : "not selected");
        continue;
      }
      if (sm.url.empty()) {
        throw_runtime_error(
            { "fetch_git: submodule '", full, "' has no url in .gitmodules" });
      }
      if (!safe_tree_path(sm.name) || !safe_tree_path(sm.path)) {
        throw_runtime_error(
            { "fetch_git: unsafe submodule '", sm.name, "' at '", sm.path, "'" });
      }
      picked.push_back(&sm);
    }

    // A name inside another's gitdir would nest one repository in another's metadata.
    if (auto const clash{ git_submodule_name_clash([&] {
          std::vector<std::string> names;
          for (auto const *sm : picked) { names.push_back(sm->name); }
          return names;
        }()) }) {
      throw_runtime_error({ "fetch_git: submodule names '",
                            clash->first,
                            "' and '",
                            clash->second,
                            "' share a gitdir" });
    }

    config_ptr const cfg{ repo_config(job.repo.get()) };

    std::vector<git_job> children;
    for (auto const *sm : picked) {
      std::string const full{ job.path.empty() ? sm->path : job.path + "/" + sm->path };
      std::string const url{ git_submodule_url(job.url, sm->url) };
      // Git's protocol.file.allow=user: a served superproject does not get to read this
      // machine's repositories by naming them.
      if (git_url_is_local(url) && !git_url_is_local(job.url)) {
        throw_runtime_error({ "fetch_git: refusing local submodule url '",
                              url,
                              "' for '",
                              full,
                              "' from remote superproject ",
                              job.url });
      }
      require_real_dirs(workdir, sm->path);

      if (git_config_set_string(cfg.get(),
                                ("submodule." + sm->name + ".url").c_str(),
                                url.c_str())) {
        throw_git_error({ "fetch_git: cannot register submodule ", full, ": " });
      }
      std::filesystem::path const child_workdir{ workdir / sm->path };
      children.push_back(git_job{
          .url = url,
          .ref = sm->sha,
          .path = full,
          .repo = init_repo(gitdir / "modules" / sm->name, &child_workdir, url) });
    }
    return children;
  }

  std::optional<glob_filter> const &filter_;
  std::string const &trace_spec_;
  git_progress progress_;

  std::mutex mutex_;  // guards everything below
  std::condition_variable cv_;
  std::deque<git_job> queue_;
  std::size_t busy_{ 0 };
  std::exception_ptr error_;
  std::vector<bool> matched_;  // per include selector, across the whole tree
};

}  // namespace

git_progress::repo *git_progress::track(std::string path) {
  std::lock_guard const lock{ mutex_ };
  repo *const r{ &repos_.emplace_back(repo{ .sum = this, .path = std::move(path) }) };
  // The local transport never reports a transfer, so here is where a stop is heard.
  if (!publish()) {
    throw_fetch_error(fetch_error_kind::ABORTED,
                      { "fetch_git: aborted before ", r->path });
  }
  return r;
}

void git_progress::finished(repo *r) {
  std::lock_guard const lock{ mutex_ };
  r->done = true;
  r->retry.reset();
  if (!r->path.empty()) { ++done_; }
  publish();
}

void git_progress::discovered(std::size_t count) {
  std::lock_guard const lock{ mutex_ };
  total_ += static_cast<std::uint32_t>(count);
  publish();
}

fetch_progress_cb_t git_progress::waits(repo *r) {
  return [this, r](fetch_progress_t const &p) {
    std::lock_guard const lock{ mutex_ };
    if (auto const *wait{ std::get_if<fetch_retry_progress>(&p) }) { r->retry = *wait; }
    return publish();
  };
}

int git_progress::on_transfer(git_indexer_progress const *stats, void *payload) {
  auto &r{ *static_cast<repo *>(payload) };
  std::lock_guard const lock{ r.sum->mutex_ };
  r.last = { .total_objects = stats->total_objects,
             .indexed_objects = stats->indexed_objects,
             .received_objects = stats->received_objects,
             .total_deltas = stats->total_deltas,
             .indexed_deltas = stats->indexed_deltas,
             .received_bytes = stats->received_bytes };
  r.retry.reset();  // bytes are moving again
  return r.sum->publish() ? 0 : GIT_EUSER;
}

void git_progress::on_checkout(char const * /*path*/,
                               std::size_t completed,
                               std::size_t total,
                               void *payload) {
  auto &r{ *static_cast<repo *>(payload) };
  std::lock_guard const lock{ r.sum->mutex_ };
  r.last.checkout_done = completed;
  r.last.checkout_total = total;
  r.sum->publish();  // libgit2 cannot stop a checkout midway; the next repository will
}

bool git_progress::publish() {
  if (row_ && !aborted_ && !row_(snapshot())) { aborted_ = true; }
  return !aborted_;
}

fetch_progress_t git_progress::snapshot() const {
  if (total_ == 0) {  // just the root so far: its fetch is the row
    repo const &root{ repos_.front() };
    return root.retry ? fetch_progress_t{ *root.retry } : fetch_progress_t{ root.last };
  }
  fetch_git_tree_progress tree{ .done = done_, .total = total_ };
  for (auto const &r : repos_) {
    tree.received_bytes += r.last.received_bytes;
    if (!r.done && !r.path.empty()) {
      tree.in_flight.push_back({ .path = r.path, .counters = r.last, .retry = r.retry });
    }
  }
  return tree;
}

git_fetch_plan const &git_fetch_first(std::vector<git_fetch_plan> const &plans,
                                      std::string_view url,
                                      std::string_view sha,
                                      function_ref<void(git_fetch_plan const &)> download,
                                      function_ref<bool()> has_commit) {
  for (auto const &plan : plans) {
    try {
      download(plan);
      // A ref that moved between advertisement and fetch brings its new tip, not ours;
      // ours is in its history, which the deeper plan after this one reaches.
      if (!has_commit()) {
        throw_fetch_error(
            fetch_error_kind::PROTOCOL,
            { "fetch_git: ", url, " sent no commit ", sha, " at depth ", plan.depth });
      }
      return plan;
    } catch (fetch_error const &e) {
      // A refusal is the server's answer to this plan; the next may not draw it.
      if (e.kind() != fetch_error_kind::PROTOCOL || &plan == &plans.back()) { throw; }
      tui::debug("fetch: %.*s refused %s at depth %d: %s",
                 static_cast<int>(url.size()),
                 url.data(),
                 git_fetch_strategy_name(plan.strategy),
                 plan.depth,
                 e.what());
    }
  }
  throw_runtime_error({ "fetch_git: no fetch plan for ", url });
}

std::optional<std::pair<std::string, std::string>> git_submodule_name_clash(
    std::vector<std::string> const &names) {
  // Compared as Windows and macOS see a path: either separator, any case.
  std::vector<std::string> keys;
  for (auto const &n : names) { keys.push_back(lowercase(glob_canonical_path(n))); }
  for (std::size_t i{ 0 }; i < keys.size(); ++i) {
    for (std::size_t j{ 0 }; j < keys.size(); ++j) {
      if (i != j && (keys[i] == keys[j] ? i < j : keys[j].starts_with(keys[i] + "/"))) {
        return std::pair{ names[i], names[j] };
      }
    }
  }
  return std::nullopt;
}

char const *git_fetch_strategy_name(git_fetch_strategy strategy) {
  switch (strategy) {
    case git_fetch_strategy::LOCAL: return "local";
    case git_fetch_strategy::TIP: return "tip";
    case git_fetch_strategy::OID: return "oid";
    case git_fetch_strategy::FULL: return "full";
  }
  return "full";
}

std::vector<git_fetch_plan> git_fetch_plans(std::string_view sha,
                                            std::vector<git_ref_entry> const &refs,
                                            bool local) {
  if (local) {
    return { { .strategy = git_fetch_strategy::LOCAL,
               .refspecs = { std::string{ sha } },
               .depth = 0 } };
  }

  // Any server sends a ref it advertises. Tags rank first: fetched by name, one comes
  // along, so `git describe` works; HEAD last, being only an alias for another ref.
  auto const rank{ [](std::string_view name) {
    return name.starts_with("refs/tags/")    ? 0
           : name.starts_with("refs/heads/") ? 1
           : name == "HEAD"                  ? 3
                                             : 2;
  } };
  std::optional<std::string> tip;
  for (auto const &r : refs) {
    if (r.oid != sha) { continue; }
    std::string_view name{ r.name };
    if (name.ends_with("^{}")) { name.remove_suffix(3); }
    if (!tip || std::pair{ rank(name), name } <
                    std::pair{ rank(*tip), std::string_view{ *tip } }) {
      tip = std::string{ name };
    }
  }

  // Every shallow plan keeps a full-depth fallback: some servers (googlesource's JGit)
  // answer libgit2's shallow negotiation with a reply it cannot parse.
  if (tip) {
    std::string const spec{ [&]() -> std::string {
      switch (rank(*tip)) {
        case 0: return "+" + *tip + ":" + *tip;
        case 1: return "+" + *tip + ":refs/remotes/origin/" + tip->substr(11);
        default: return *tip;  // FETCH_HEAD only
      }
    }() };
    return { { .strategy = git_fetch_strategy::TIP, .refspecs = { spec }, .depth = 1 },
             { .strategy = git_fetch_strategy::TIP, .refspecs = { spec }, .depth = 0 } };
  }

  return { { .strategy = git_fetch_strategy::OID,
             .refspecs = { std::string{ sha } },
             .depth = 1 },
           { .strategy = git_fetch_strategy::FULL,
             .refspecs = { "+refs/heads/*:refs/remotes/origin/*",
                           "+refs/tags/*:refs/tags/*" },
             .depth = 0 } };
}

std::string git_fetch_target(std::string_view ref,
                             std::vector<git_ref_entry> const &refs) {
  if (git_ref_is_full_sha(ref)) { return lowercase(ref); }
  try {
    return git_resolve_ref(refs, ref);
  } catch (std::exception const &e) {
    throw_fetch_error(fetch_error_kind::PROTOCOL,
                      { e.what(),
                        "; a git ref must be a full commit sha or an advertised branch "
                        "or tag (`envy git-resolve` pins one)" });
  }
}

bool git_url_is_local(std::string_view url) {
  if (url.starts_with("file://")) { return true; }
  if (url.find("://") != std::string_view::npos) { return false; }
  auto const colon{ url.find(':') };
  if (colon == std::string_view::npos) { return true; }
  if (colon == 1 && std::isalpha(static_cast<unsigned char>(url[0]))) { return true; }
  return url.find('/') < colon;  // "a/b:c" is a path; "host:path" is scp-style ssh
}

std::string git_submodule_url(std::string_view super, std::string_view url) {
  if (!url.starts_with("./") && !url.starts_with("../")) { return std::string{ url }; }

  std::string base{ super };
  if (base.ends_with('/')) { base.pop_back(); }

  // "../" may climb to the host, never into "scheme://host" itself.
  auto const scheme{ base.find("://") };
  auto const floor{ scheme == std::string::npos ? std::string::npos
                                                : base.find('/', scheme + 3) };
  bool colon{ false };  // scp-style: the path rejoins the host with ':'
  for (;;) {
    if (url.starts_with("./")) {
      url.remove_prefix(2);
    } else if (url.starts_with("../")) {
      url.remove_prefix(3);
      auto const slash{ base.rfind('/') };
      if (scheme != std::string::npos && (floor == std::string::npos || slash < floor)) {
        throw_runtime_error({ "fetch_git: submodule url climbs above ", super });
      }
      if (slash != std::string::npos) {
        base.resize(slash);
      } else if (auto const c{ base.rfind(':') }; c != std::string::npos) {
        base.resize(c);
        colon = true;
      } else {
        throw_runtime_error({ "fetch_git: submodule url climbs above ", super });
      }
    } else {
      break;
    }
  }

  std::string out{ base + (colon ? ":" : "/") + std::string{ url } };
  if (out.ends_with('/')) { out.pop_back(); }
  return out;
}

fetch_error_kind git_error_kind(int klass, std::string_view message) {
  // libgit2 reports HTTP statuses only as text. A 4xx other than 429 is a statement about
  // the request, as on the http transport; a reply it cannot parse will parse no better.
  constexpr std::string_view kStatus{ "unexpected http status code: " };
  int const status{ message.starts_with(kStatus)
                        ? std::atoi(std::string{ message.substr(kStatus.size()) }.c_str())
                        : 0 };
  if (message.starts_with("remote error:") || message.starts_with("invalid packet line") ||
      message.starts_with("server doesn't support") ||
      message.find("authentication required") != std::string_view::npos ||
      (status >= 400 && status < 500 && status != 429)) {
    return fetch_error_kind::PROTOCOL;
  }
  if (message.find("timed out") != std::string_view::npos) {  // a bound we set ran out
    return message.starts_with("failed to connect") ? fetch_error_kind::CONNECT
                                                    : fetch_error_kind::TIMEOUT;
  }
  switch (klass) {
    case GIT_ERROR_NET:
    case GIT_ERROR_HTTP:
    case GIT_ERROR_INDEXER: return fetch_error_kind::TRANSFER;
    // INVALID is libgit2 1.9 declining, before sending, a want the server cannot take.
    case GIT_ERROR_INVALID:
    case GIT_ERROR_REFERENCE: return fetch_error_kind::PROTOCOL;
    default: return fetch_error_kind::OTHER;
  }
}

void fetch_git(fetch_request_git const &req,
               std::filesystem::path const &dest,
               std::string const &trace_spec) {
  // Absolute, so relative submodule urls resolve the same from any working directory.
  std::string url{
    git_url_is_local(req.source) && !req.source.starts_with("file://")
        ? std::filesystem::absolute(req.source).lexically_normal().generic_string()
        : req.source
  };
  git_tree{ req, trace_spec }.run(dest, std::move(url), req.ref);
}

}  // namespace envy
