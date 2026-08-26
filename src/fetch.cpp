#include "fetch.h"

#include "aws_util.h"
#include "fetch_error.h"
#include "fetch_http.h"
#include "libgit2_util.h"
#include "trace.h"
#include "tui.h"
#include "util.h"

#include "git2.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace envy {
namespace {

std::filesystem::path prepare_destination(std::filesystem::path destination) {
  if (destination.empty()) {
    throw std::invalid_argument("fetch: destination path is empty");
  }

  if (!destination.is_absolute()) { destination = std::filesystem::absolute(destination); }
  destination = destination.lexically_normal();

  if (auto const parent{ destination.parent_path() }; !parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      throw std::runtime_error("fetch: failed to create destination parent: " +
                               parent.string() + ": " + ec.message());
    }
  }

  return destination;
}

std::filesystem::path resolve_file_path(
    std::string const &canonical_path,
    std::optional<std::filesystem::path> const &file_root) {
  std::filesystem::path source{ canonical_path };
  return std::filesystem::absolute(source.is_relative() && file_root ? *file_root / source
                                                                     : source)
      .lexically_normal();
}

fetch_result fetch_local_file(std::string const &canonical_path,
                              std::filesystem::path const &destination,
                              std::optional<std::filesystem::path> const &file_root) {
  auto const source{ resolve_file_path(canonical_path, file_root) };

  // Validate source exists
  std::error_code ec;
  if (!std::filesystem::exists(source, ec)) {
    throw std::runtime_error("fetch: source file does not exist: " + source.string());
  }
  if (ec) {
    throw std::runtime_error("fetch: failed to check source: " + source.string() + ": " +
                             ec.message());
  }

  auto const dest{ prepare_destination(destination) };

  bool const is_directory{ std::filesystem::is_directory(source, ec) };
  if (ec) {
    throw std::runtime_error("fetch: failed to check if source is directory: " +
                             source.string() + ": " + ec.message());
  }

  if (is_directory) {
    std::filesystem::copy(source,
                          dest,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          ec);
    if (ec) {
      throw std::runtime_error("fetch: failed to copy directory: " + source.string() +
                               " -> " + dest.string() + ": " + ec.message());
    }
  } else {
    std::filesystem::copy_file(source,
                               dest,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
      throw std::runtime_error("fetch: failed to copy file: " + source.string() + " -> " +
                               dest.string() + ": " + ec.message());
    }
  }

  return fetch_result{ .scheme = uri_scheme::LOCAL_FILE_ABSOLUTE,
                       .resolved_source = source,
                       .resolved_destination = dest };
}

int git_fetch_progress_callback(git_indexer_progress const *stats, void *payload) {
  auto *cb{ static_cast<fetch_progress_cb_t *>(payload) };
  if (!cb || !*cb) { return 0; }

  fetch_git_progress progress{
    .total_objects = stats->total_objects,
    .indexed_objects = stats->indexed_objects,
    .received_objects = stats->received_objects,
    .total_deltas = stats->total_deltas,
    .indexed_deltas = stats->indexed_deltas,
    .received_bytes = stats->received_bytes,
  };

  return (*cb)(progress) ? 0 : -1;
}

// Attempt git clone with optional shallow depth. Returns nullptr on failure (no throw).
git_repository *try_git_clone(std::string const &url,
                              std::filesystem::path const &dest,
                              fetch_progress_cb_t const &progress,
                              int depth) {
  git_clone_options const clone_opts{ [&] {
    git_clone_options o;
    git_clone_options_init(&o, GIT_CLONE_OPTIONS_VERSION);
    if (depth > 0) { o.fetch_opts.depth = depth; }
    o.fetch_opts.callbacks.transfer_progress = git_fetch_progress_callback;
    o.fetch_opts.callbacks.payload = const_cast<fetch_progress_cb_t *>(&progress);
    return o;
  }() };

  git_repository *repo_raw{ nullptr };
  if (git_clone(&repo_raw, url.c_str(), dest.string().c_str(), &clone_opts)) {
    return nullptr;
  }
  return repo_raw;
}

// libgit2 reports the failing subsystem in git_error::klass, the only signal that
// separates "the network dropped" from "that ref does not exist". `context` ends in
// ": " so the library's own message reads as the tail of the sentence.
[[noreturn]] void throw_git_error(std::string const &context) {
  git_error const *err{ git_error_last() };

  auto const kind{ [klass = err ? err->klass : GIT_ERROR_NONE] {
    switch (klass) {
      case GIT_ERROR_NET:
      case GIT_ERROR_HTTP:
      case GIT_ERROR_INDEXER: return fetch_error_kind::TRANSFER;
      case GIT_ERROR_REFERENCE: return fetch_error_kind::PROTOCOL;
      default: return fetch_error_kind::OTHER;
    }
  }() };

  throw fetch_error(kind, err ? context + err->message : context);
}

// Try to resolve ref in repo. Returns nullptr on failure (no throw).
git_object *try_resolve_ref(git_repository *repo, std::string const &ref) {
  if (git_object *obj{ nullptr }; !git_revparse_single(&obj, repo, ref.c_str())) {
    return obj;
  }
  return nullptr;
}

fetch_result fetch_git_repo(std::string const &url,
                            std::string const &ref,
                            std::filesystem::path const &destination,
                            fetch_progress_cb_t const &progress,
                            uri_scheme scheme) {
  if (scheme == uri_scheme::GIT_HTTPS) { libgit2_require_ssl_certs(); }
  auto const dest{ prepare_destination(destination) };

  // git_clone rejects a non-empty target, and a failed clone leaves its partial work
  // behind, so every attempt -- first or retry -- starts from a clean slate.
  std::error_code ec;
  std::filesystem::remove_all(dest, ec);

  // Try shallow clone first; fall back to full clone if shallow fails or ref not found.
  // Some servers (e.g., googlesource.com) have libgit2 shallow clone issues.
  // Shallow clones also may not fetch all tags, causing ref resolution to fail.
  git_repository *repo_raw{ try_git_clone(url, dest, progress, 1) };
  git_object *target_obj{ nullptr };
  bool need_full_clone{ !repo_raw };

  if (repo_raw) {
    target_obj = try_resolve_ref(repo_raw, ref);
    if (!target_obj) {
      // Shallow clone succeeded but ref not found - need full clone
      git_repository_free(repo_raw);
      repo_raw = nullptr;
      need_full_clone = true;
    }
  }

  if (need_full_clone) {
    std::filesystem::remove_all(dest, ec);
    std::filesystem::create_directories(dest, ec);

    repo_raw = try_git_clone(url, dest, progress, 0);
    if (!repo_raw) { throw_git_error("fetch_git: clone failed: "); }

    target_obj = try_resolve_ref(repo_raw, ref);
    if (!target_obj) {
      // The clone succeeded, so whatever git_error_last() holds is stale: a ref the
      // remote does not publish is a fact about the request, never a transport fault.
      git_repository_free(repo_raw);
      git_error const *git_err{ git_error_last() };
      throw fetch_error(fetch_error_kind::PROTOCOL,
                        "fetch_git: failed to resolve ref '" + ref +
                            "': " + (git_err ? git_err->message : ""));
    }
  }

  std::unique_ptr<git_repository, decltype(&git_repository_free)> repo{
    repo_raw,
    git_repository_free
  };

  std::unique_ptr<git_object, decltype(&git_object_free)> target{ target_obj,
                                                                  git_object_free };

  git_checkout_options const checkout_opts{ [] {
    git_checkout_options o;
    git_checkout_options_init(&o, GIT_CHECKOUT_OPTIONS_VERSION);
    o.checkout_strategy = GIT_CHECKOUT_FORCE;
    return o;
  }() };

  if (git_checkout_tree(repo.get(), target.get(), &checkout_opts)) {
    git_error const *git_err{ git_error_last() };
    std::string msg{ "fetch_git: checkout failed: " };
    if (git_err) { msg += git_err->message; }
    throw std::runtime_error(msg);
  }

  // Update HEAD to point to the target (detached HEAD state)
  git_oid const *target_oid{ git_object_id(target.get()) };
  if (git_repository_set_head_detached(repo.get(), target_oid)) {
    git_error const *git_err{ git_error_last() };
    std::string msg{ "fetch_git: failed to update HEAD: " };
    if (git_err) { msg += git_err->message; }
    throw std::runtime_error(msg);
  }

  return fetch_result{ .scheme = scheme,
                       .resolved_source = std::filesystem::path{ url },
                       .resolved_destination = dest };
}

}  // namespace

fetch_result fetch_single(fetch_request const &request) {
  auto const fetch_http{ [](auto const &req) -> fetch_result {
    auto const info{ uri_classify(req.source) };
    if (info.canonical.empty() && info.scheme == uri_scheme::UNKNOWN) {
      throw std::invalid_argument("fetch: source URI is empty");
    }
    return fetch_result{
      .scheme = info.scheme,
      .resolved_source = std::filesystem::path{ info.canonical },
      .resolved_destination =
          fetch_http_download(info.canonical, req.destination, req.progress, req.post_data)
    };
  } };

  auto const fetch_ftp{ [](auto const &req) -> fetch_result {
    auto const info{ uri_classify(req.source) };
    if (info.canonical.empty() && info.scheme == uri_scheme::UNKNOWN) {
      throw std::invalid_argument("fetch: source URI is empty");
    }
    return fetch_result{
      .scheme = info.scheme,
      .resolved_source = std::filesystem::path{ info.canonical },
      .resolved_destination =
          fetch_http_download(info.canonical, req.destination, req.progress, std::nullopt)
    };
  } };

  return std::visit(
      match{
          [&](fetch_request_http const &req) { return fetch_http(req); },
          [&](fetch_request_https const &req) { return fetch_http(req); },
          [&](fetch_request_ftp const &req) { return fetch_ftp(req); },
          [&](fetch_request_ftps const &req) { return fetch_ftp(req); },
          [](fetch_request_s3 const &req) -> fetch_result {
            auto const info{ uri_classify(req.source) };
            if (info.canonical.empty() && info.scheme == uri_scheme::UNKNOWN) {
              throw std::invalid_argument("fetch: source URI is empty");
            }
            return fetch_result{ .scheme = info.scheme,
                                 .resolved_source =
                                     std::filesystem::path{ info.canonical },
                                 .resolved_destination = aws_s3_download(
                                     s3_download_request{ .uri = info.canonical,
                                                          .destination = req.destination,
                                                          .region = req.region,
                                                          .progress = req.progress }) };
          },
          [](fetch_request_file const &req) -> fetch_result {
            auto const info{ uri_classify(req.source) };
            if (info.canonical.empty() && info.scheme == uri_scheme::UNKNOWN) {
              throw std::invalid_argument("fetch: source URI is empty");
            }
            return fetch_local_file(info.canonical, req.destination, req.file_root);
          },
          [](fetch_request_git const &req) -> fetch_result {
            return fetch_git_repo(req.source,
                                  req.ref,
                                  req.destination,
                                  req.progress,
                                  req.scheme);
          },
      },
      request);
}

namespace {

struct retry_policy {
  int attempts;
  std::chrono::milliseconds base_delay;
};

// A retry that waits longer than the transfer it is retrying helps nobody.
constexpr std::chrono::milliseconds kMaxRetryDelay{ 60000 };

int env_int(char const *name, int fallback, int lo, int hi) {
  char const *val{ std::getenv(name) };
  if (!val || !*val) { return fallback; }
  char *end{ nullptr };
  long const parsed{ std::strtol(val, &end, 10) };
  return (*end == '\0' && parsed >= lo && parsed <= hi) ? static_cast<int>(parsed)
                                                        : fallback;
}

// Read once: the knobs exist for CI and for tests that cannot afford real backoff,
// and re-reading the environment per attempt would only invite it to change mid-fetch.
retry_policy const &fetch_retry_policy() {
  static retry_policy const policy{
    .attempts = env_int("ENVY_FETCH_ATTEMPTS", 3, 1, 10),
    .base_delay =
        std::chrono::milliseconds{ env_int("ENVY_FETCH_RETRY_BASE_MS", 1000, 0, 60000) }
  };
  return policy;
}

// Exponential (1x, 4x, 16x base) and jittered over +/-50%. fetch() runs a thread per
// request, so without the jitter a batch that all failed against the same bad mirror
// would march back onto it in lockstep.
std::chrono::milliseconds retry_delay(int attempt, std::chrono::milliseconds base) {
  if (base.count() <= 0) { return std::chrono::milliseconds::zero(); }

  auto const scaled{ std::min(base.count() << std::min(2 * (attempt - 1), 20),
                              kMaxRetryDelay.count()) };

  static thread_local std::mt19937 rng{ std::random_device{}() };
  std::uniform_real_distribution<double> jitter{ 0.5, 1.5 };
  return std::chrono::milliseconds{ static_cast<std::chrono::milliseconds::rep>(
      static_cast<double>(scaled) * jitter(rng)) };
}

// The single retry seam for every scheme. Retrying here rather than inside a backend
// means http, ftp, s3 and git all get one policy, and each attempt re-runs the
// backend's own setup -- including the destination wipe that a partial left behind.
// Safe because fetches are idempotent GETs and payloads are sha256-verified by the
// caller after transport, so a replay can never launder bad bytes.
fetch_result fetch_with_retry(fetch_request const &request,
                              std::string const &trace_spec,
                              std::string const &url) {
  auto const &policy{ fetch_retry_policy() };

  for (int attempt{ 1 };; ++attempt) {
    try {
      return fetch_single(request);
    } catch (fetch_error const &e) {
      if (attempt >= policy.attempts || !fetch_error_retryable(e)) { throw; }

      auto const delay{ retry_delay(attempt, policy.base_delay) };
      tui::debug("fetch: attempt %d of %d failed (%s), retrying in %lldms: %s",
                 attempt,
                 policy.attempts,
                 fetch_error_kind_name(e.kind()),
                 static_cast<long long>(delay.count()),
                 e.what());
      ENVY_TRACE(download_retry,
                 trace_spec,
                 .url = url,
                 .attempt = attempt,
                 .delay_ms = static_cast<std::int64_t>(delay.count()),
                 .reason = fetch_error_kind_name(e.kind()),
                 .error = e.what());
      std::this_thread::sleep_for(delay);
    }
  }
}

}  // namespace

std::vector<fetch_result_t> fetch(std::vector<fetch_request> const &requests,
                                  std::string trace_spec) {
  std::vector<fetch_result_t> results(requests.size());

  std::vector<std::thread> workers;
  workers.reserve(requests.size());

  for (size_t i = 0; i < requests.size(); ++i) {
    workers.emplace_back([i, &requests, &results, &trace_spec]() {
      // The source URL names the retry target, so it is always needed; the rest is
      // trace-only work gated on trace_enabled so a disabled stream costs nothing here.
      std::string const source{ std::visit([](auto const &r) { return r.source; },
                                           requests[i]) };
      bool const tracing{ tui::trace_enabled() };
      if (tracing) {
        ENVY_TRACE(
            download_start,
            trace_spec,
            .url = source,
            .destination = std::visit([](auto const &r) { return r.destination.string(); },
                                      requests[i]));
      }
      auto const start{ std::chrono::steady_clock::now() };

      try {
        results[i] = fetch_with_retry(requests[i], trace_spec, source);

        if (tracing) {
          auto const &res{ std::get<fetch_result>(results[i]) };
          std::error_code size_ec;
          auto const bytes{
            std::filesystem::is_regular_file(res.resolved_destination, size_ec)
                ? std::filesystem::file_size(res.resolved_destination, size_ec)
                : 0
          };
          ENVY_TRACE(download_complete,
                     trace_spec,
                     .url = source,
                     .bytes = static_cast<std::int64_t>(size_ec ? 0 : bytes),
                     .duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count());
        }
      } catch (std::exception const &e) {
        results[i] = std::string(e.what());
      } catch (...) { results[i] = "Unknown error during fetch"; }

      if (tracing) {
        if (auto const *error{ std::get_if<std::string>(&results[i]) }) {
          ENVY_TRACE(download_failed, trace_spec, .url = source, .error = *error);
        }
      }
    });
  }

  for (auto &t : workers) { t.join(); }

  return results;
}

fetch_request fetch_request_from_url(std::string const &url,
                                     std::filesystem::path const &dest) {
  auto const info{ uri_classify(url) };
  switch (info.scheme) {
    case uri_scheme::HTTP: return fetch_request_http{ .source = url, .destination = dest };
    case uri_scheme::HTTPS:
      return fetch_request_https{ .source = url, .destination = dest };
    case uri_scheme::FTP: return fetch_request_ftp{ .source = url, .destination = dest };
    case uri_scheme::FTPS: return fetch_request_ftps{ .source = url, .destination = dest };
    case uri_scheme::S3: return fetch_request_s3{ .source = url, .destination = dest };
    case uri_scheme::LOCAL_FILE_ABSOLUTE:
    case uri_scheme::LOCAL_FILE_RELATIVE:
      return fetch_request_file{ .source = url, .destination = dest };
    default: throw std::runtime_error("Unsupported URL scheme: " + url);
  }
}

}  // namespace envy
