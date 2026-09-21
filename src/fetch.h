#pragma once

#include "uri.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace envy {

struct fetch_transfer_progress {
  std::uint64_t transferred{ 0 };
  std::optional<std::uint64_t> total;
};

struct fetch_git_progress {
  std::uint32_t total_objects{ 0 };
  std::uint32_t indexed_objects{ 0 };
  std::uint32_t received_objects{ 0 };
  std::uint32_t total_deltas{ 0 };
  std::uint32_t indexed_deltas{ 0 };
  std::uint64_t received_bytes{ 0 };
};

// A wait with nothing moving. Without a report of its own a backoff reads as a stall:
// the row would hold its last byte count for as long as the whole budget.
struct fetch_retry_progress {
  int attempt{ 0 };  // the attempt that just failed, not the one being waited for
  std::chrono::milliseconds remaining{ 0 };
  char const *reason{ "" };  // fetch_error_kind_name of the failure; always a literal
};

using fetch_progress_t =
    std::variant<fetch_transfer_progress, fetch_git_progress, fetch_retry_progress>;
using fetch_progress_cb_t = std::function<bool(fetch_progress_t const &)>;

struct http_tag {};
struct https_tag {};

template <typename SchemeTag>
struct http_request {
  std::string source;
  std::filesystem::path destination;
  fetch_progress_cb_t progress{};
  std::optional<std::string> post_data;
};

using fetch_request_http = http_request<http_tag>;
using fetch_request_https = http_request<https_tag>;

struct ftp_tag {};
struct ftps_tag {};

template <typename SchemeTag>
struct ftp_request {
  std::string source;
  std::filesystem::path destination;
  fetch_progress_cb_t progress{};
};

using fetch_request_ftp = ftp_request<ftp_tag>;
using fetch_request_ftps = ftp_request<ftps_tag>;

// S3 fetch request with region
struct fetch_request_s3 {
  std::string source;
  std::filesystem::path destination;
  fetch_progress_cb_t progress{};
  std::string region;
};

// Local file fetch request with file_root
struct fetch_request_file {
  std::string source;
  std::filesystem::path destination;
  fetch_progress_cb_t progress{};
  std::filesystem::path file_root;
};

// Git fetch request with ref (committish: tag, branch, or SHA)
// Note: Submodules are not yet supported
struct fetch_request_git {
  std::string source;
  std::filesystem::path destination;
  fetch_progress_cb_t progress{};
  std::string ref;
  uri_scheme scheme{ uri_scheme::GIT };  // GIT or GIT_HTTPS
};

using fetch_request = std::variant<fetch_request_http,
                                   fetch_request_https,
                                   fetch_request_ftp,
                                   fetch_request_ftps,
                                   fetch_request_s3,
                                   fetch_request_file,
                                   fetch_request_git>;

struct fetch_result {
  uri_scheme scheme;
  std::filesystem::path resolved_source;
  std::filesystem::path resolved_destination;
};

using fetch_result_t = std::variant<fetch_result, std::string>;  // string on error

// trace_spec labels emitted download_* trace events with the requesting package
// identity (empty = engine/command-scoped).
std::vector<fetch_result_t> fetch(std::vector<fetch_request> const &requests,
                                  std::string trace_spec = {});

// Build a fetch_request from a URL + destination. Handles HTTP, HTTPS, FTP, FTPS, S3,
// and local files. Throws on git, SSH, or unknown schemes.
fetch_request fetch_request_from_url(std::string const &url,
                                     std::filesystem::path const &dest);

}  // namespace envy
