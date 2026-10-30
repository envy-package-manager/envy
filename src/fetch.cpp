#include "fetch.h"

#include "aws_util.h"
#include "fetch_error.h"
#include "fetch_git.h"
#include "fetch_http.h"
#include "trace.h"
#include "tree_hash.h"
#include "tui.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <variant>
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
      throw_runtime_error(
          { "fetch: failed to create destination parent: ", parent, ": ", ec.message() });
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
    throw_runtime_error({ "fetch: source file does not exist: ", source });
  }
  if (ec) {
    throw_runtime_error({ "fetch: failed to check source: ", source, ": ", ec.message() });
  }

  auto const dest{ prepare_destination(destination) };

  bool const is_directory{ std::filesystem::is_directory(source, ec) };
  if (ec) {
    throw_runtime_error(
        { "fetch: failed to check if source is directory: ", source, ": ", ec.message() });
  }

  if (is_directory) {
    std::filesystem::copy(source,
                          dest,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          ec);
    if (ec) {
      throw_runtime_error({ "fetch: failed to copy directory: ",
                            source,
                            " -> ",
                            dest,
                            ": ",
                            ec.message() });
    }
  } else {
    std::filesystem::copy_file(source,
                               dest,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
      throw_runtime_error(
          { "fetch: failed to copy file: ", source, " -> ", dest, ": ", ec.message() });
    }
  }

  return fetch_result{ .scheme = uri_scheme::LOCAL_FILE_ABSOLUTE,
                       .resolved_source = source,
                       .resolved_destination = dest };
}

fetch_result fetch_single(fetch_request const &request, std::string const &trace_spec) {
  // Every scheme resolves its source the same way, and an unresolvable one is a caller
  // bug, not a transport failure.
  auto const classify{ [](std::string const &source) {
    auto const info{ uri_classify(source) };
    if (info.canonical.empty() && info.scheme == uri_scheme::UNKNOWN) {
      throw std::invalid_argument("fetch: source URI is empty");
    }
    return info;
  } };

  // Each attempt re-runs the backend's own setup, including the destination wipe.
  auto const retrying{ [&](auto const &req, auto const &attempt) -> fetch_result {
    std::optional<fetch_result> result;
    fetch_retrying([&] { result = attempt(); }, req.progress, trace_spec, req.source);
    return std::move(*result);
  } };

  auto const download{ [&](auto const &req, std::optional<std::string> const &post_data) {
    return retrying(req, [&] {
      auto const info{ classify(req.source) };
      return fetch_result{
        .scheme = info.scheme,
        .resolved_source = std::filesystem::path{ info.canonical },
        .resolved_destination =
            fetch_http_download(info.canonical, req.destination, req.progress, post_data)
      };
    });
  } };

  return envy::visit(
      match{
          [&](fetch_request_http const &req) { return download(req, req.post_data); },
          [&](fetch_request_https const &req) { return download(req, req.post_data); },
          [&](fetch_request_ftp const &req) { return download(req, std::nullopt); },
          [&](fetch_request_ftps const &req) { return download(req, std::nullopt); },
          // The AWS SDK's TransferManager retries internally.
          [&](fetch_request_s3 const &req) -> fetch_result {
            auto const info{ classify(req.source) };
            return fetch_result{ .scheme = info.scheme,
                                 .resolved_source =
                                     std::filesystem::path{ info.canonical },
                                 .resolved_destination = aws_s3_download(
                                     s3_download_request{ .uri = info.canonical,
                                                          .destination = req.destination,
                                                          .region = req.region,
                                                          .progress = req.progress }) };
          },
          [&](fetch_request_file const &req) -> fetch_result {
            return fetch_local_file(classify(req.source).canonical,
                                    req.destination,
                                    req.file_root);
          },
          // Retried per repository inside: one blip must not restart a submodule tree.
          [&](fetch_request_git const &req) -> fetch_result {
            auto const dest{ prepare_destination(req.destination) };
            tree_remove_best_effort(dest);
            fetch_git(req, dest, trace_spec);
            return fetch_result{ .scheme = req.scheme,
                                 .resolved_source = std::filesystem::path{ req.source },
                                 .resolved_destination = dest };
          },
      },
      request);
}

struct retry_policy {
  int attempts;  // hard ceiling; the time budget is what normally binds
  std::chrono::milliseconds base_delay;
  // Elapsed from the first failure, not time asleep: an attempt that burns its own
  // 30-second connect timeout has spent the budget as surely as a wait would.
  std::chrono::milliseconds budget;
};

// A single wait longer than this buys nothing a shorter one plus another attempt does not.
constexpr std::chrono::milliseconds kMaxRetryDelay{ 30000 };

// A host that will not finish a handshake is down rather than busy, and rarely recovers
// inside a minute. Throttling and 5xx storms do, so only those earn the full budget.
constexpr std::chrono::milliseconds kConnectBudget{ 5000 };

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
    .attempts = env_int("ENVY_FETCH_ATTEMPTS", 10, 1, 100),
    .base_delay =
        std::chrono::milliseconds{ env_int("ENVY_FETCH_RETRY_BASE_MS", 1000, 0, 60000) },
    .budget =
        std::chrono::milliseconds{ env_int("ENVY_FETCH_BUDGET_MS", 90000, 0, 3600000) }
  };
  return policy;
}

// fetch() runs a thread per request, so an unjittered wait marches a batch that all
// failed against one bad mirror straight back onto it in lockstep.
std::chrono::milliseconds jittered(std::chrono::milliseconds value, double lo, double hi) {
  static thread_local std::mt19937 rng{ std::random_device{}() };
  std::uniform_real_distribution<double> spread{ lo, hi };
  return std::chrono::milliseconds{ static_cast<std::chrono::milliseconds::rep>(
      static_cast<double>(value.count()) * spread(rng)) };
}

// Exponential (1x, 2x, 4x ... of base) over +/-50%. Doubling rather than quadrupling
// keeps several attempts inside the first few seconds, where transient faults live.
std::chrono::milliseconds retry_delay(int attempt, std::chrono::milliseconds base) {
  if (base.count() <= 0) { return std::chrono::milliseconds::zero(); }
  auto const scaled{ base.count() << std::min(attempt - 1, 20) };
  return std::min(kMaxRetryDelay, jittered(std::chrono::milliseconds{ scaled }, 0.5, 1.5));
}

// How long to wait before the next attempt, or nullopt for "stop trying": the budget is
// spent, or the server asked for longer than it has left, and coming back early against
// an explicit Retry-After would only earn the same answer.
std::optional<std::chrono::milliseconds> next_delay(int attempt,
                                                    std::chrono::milliseconds remaining,
                                                    fetch_error const &err,
                                                    retry_policy const &policy) {
  if (remaining <= std::chrono::milliseconds::zero()) { return std::nullopt; }

  auto const curve{ retry_delay(attempt, policy.base_delay) };
  auto const asked{ err.retry_after().value_or(std::chrono::seconds::zero()) };
  if (asked <= std::chrono::seconds::zero()) { return std::min(curve, remaining); }

  // Retry-After is a floor, not an override. It is the one wait every thread takes from
  // the same host at the same instant, so it is jittered upward -- never back below what
  // the server asked for -- rather than handed out verbatim to a whole batch.
  auto const floor{ std::chrono::duration_cast<std::chrono::milliseconds>(asked) };
  if (floor > remaining) { return std::nullopt; }
  return std::clamp(std::max(curve, jittered(floor, 1.0, 1.5)), floor, remaining);
}

// Sliced so the row can count the wait down, and so an abort does not have to sit
// through a 30-second nap. False means the caller asked to stop.
bool sleep_reporting(std::chrono::milliseconds delay,
                     fetch_progress_cb_t const &progress,
                     int attempt,
                     char const *reason) {
  constexpr std::chrono::milliseconds kTick{ 250 };
  auto const deadline{ std::chrono::steady_clock::now() + delay };

  for (auto now{ std::chrono::steady_clock::now() }; now < deadline;
       now = std::chrono::steady_clock::now()) {
    auto const remaining{ std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                                now) };
    if (progress &&
        !progress(fetch_progress_t{ std::in_place_type<fetch_retry_progress>,
                                    fetch_retry_progress{ .attempt = attempt,
                                                          .remaining = remaining,
                                                          .reason = reason } })) {
      return false;
    }
    std::this_thread::sleep_for(std::min(kTick, remaining));
  }
  return true;
}

}  // namespace

// The single retry seam. Retrying here rather than inside a backend gives http, ftp
// and git one policy; git calls in once per repository, so a tree of submodules never
// restarts whole. s3 is deliberately absent: the AWS SDK's TransferManager already
// retries internally. Safe because fetches are idempotent GETs and payloads are
// sha256-verified by the caller after transport, so a replay can never launder bad bytes.
void fetch_retrying(function_ref<void()> once,
                    fetch_progress_cb_t const &progress,
                    std::string const &trace_spec,
                    std::string const &url) {
  auto const &policy{ fetch_retry_policy() };

  // Anchored on the first failure rather than on entry, so a long transfer that succeeds
  // late is never charged for a budget it did not need.
  std::optional<std::chrono::steady_clock::time_point> first_failure;

  for (int attempt{ 1 };; ++attempt) {
    try {
      return once();
    } catch (fetch_error const &e) {
      if (attempt >= policy.attempts || !fetch_error_retryable(e)) { throw; }

      auto const now{ std::chrono::steady_clock::now() };
      if (!first_failure) { first_failure = now; }

      // Read off the failure in hand, so a run that starts with a dead host and then
      // finds a throttling one earns the longer budget from that point.
      auto const budget{ e.kind() == fetch_error_kind::CONNECT
                             ? std::min(policy.budget, kConnectBudget)
                             : policy.budget };
      auto const remaining{ std::chrono::duration_cast<std::chrono::milliseconds>(
          *first_failure + budget - now) };

      auto const delay{ next_delay(attempt, remaining, e, policy) };
      if (!delay) { throw; }

      tui::debug("fetch: attempt %d failed (%s), retrying in %lldms: %s",
                 attempt,
                 fetch_error_kind_name(e.kind()),
                 static_cast<long long>(delay->count()),
                 e.what());
      ENVY_TRACE(download_retry,
                 trace_spec,
                 .url = url,
                 .attempt = attempt,
                 .delay_ms = static_cast<std::int64_t>(delay->count()),
                 .reason = fetch_error_kind_name(e.kind()),
                 .error = e.what());

      // A cancel during the wait is a cancel, not the transport failure that preceded it:
      // rethrowing `e` would report a retryable 503 where the caller asked to stop.
      if (!sleep_reporting(*delay, progress, attempt, fetch_error_kind_name(e.kind()))) {
        throw fetch_error(fetch_error_kind::ABORTED,
                          "fetch: aborted while waiting to retry " + url);
      }
    }
  }
}

std::vector<fetch_result_t> fetch(std::vector<fetch_request> const &requests,
                                  std::string trace_spec) {
  std::vector<fetch_result_t> results(requests.size());

  std::vector<std::thread> workers;
  workers.reserve(requests.size());

  // Read on the calling thread: the engine's per-package context is thread-local, so
  // every worker below has to reopen it or its log lines come out unattributed.
  std::string const log_ctx{ tui::log_ctx() };

  for (size_t i = 0; i < requests.size(); ++i) {
    workers.emplace_back([i, &requests, &results, &trace_spec, &log_ctx]() {
      tui::log_ctx_scope const worker_log_ctx{ log_ctx };

      // The source URL names the retry target, so it is always needed; the rest is
      // trace-only work gated on trace_enabled so a disabled stream costs nothing here.
      std::string const source{ envy::visit([](auto const &r) { return r.source; },
                                            requests[i]) };
      bool const tracing{ tui::trace_enabled() };
      if (tracing) {
        ENVY_TRACE(download_start,
                   trace_spec,
                   .url = source,
                   .destination =
                       envy::visit([](auto const &r) { return r.destination.string(); },
                                   requests[i]));
      }
      auto const start{ std::chrono::steady_clock::now() };

      try {
        results[i] = fetch_single(requests[i], trace_spec);

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
