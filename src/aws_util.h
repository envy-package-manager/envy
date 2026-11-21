#pragma once

#include "fetch.h"
#include "util.h"

#include <condition_variable>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace envy {

void aws_shutdown();  // Joins aws-c-* threads; envy's last AWS call.

struct s3_uri_parts {
  std::string bucket;
  std::string key;  // empty when the URI names only a bucket
};

// Splits s3://bucket[/key]. The scheme match is case-insensitive because uri_classify
// accepts S3:// too. A trailing '/' is stripped from the key so callers can join with '/'
// without minting a distinct double-slash key -- S3 keys are opaque bytes, so "a//b" and
// "a/b" are different objects. op appears in error messages only.
s3_uri_parts aws_s3_parse_uri(std::string_view uri, std::string_view op);

// Where one object's requests go: scheme://authority is the connection and Host header,
// path the URI-encoded request path.
struct s3_endpoint {
  std::string scheme;
  std::string authority;
  std::string path;
};

// Virtual-hosted unless the bucket can't be a TLS host label or override_url (the
// AWS_ENDPOINT_URL[_S3] value) is an IP or localhost; then path-style.
s3_endpoint aws_s3_endpoint(s3_uri_parts const &parts,
                            std::string_view region,
                            std::string_view override_url);

struct s3_error {
  std::string code;
  std::string message;
};

// <Code> and <Message> of an S3 XML error body, unescaped; empty where absent.
s3_error aws_s3_parse_error(std::string_view xml);

struct s3_download_request {
  std::string uri;
  std::filesystem::path destination;
  std::optional<std::string> region;
  fetch_progress_cb_t progress{};
};

std::filesystem::path aws_s3_download(s3_download_request const &request);

struct s3_upload_request {
  std::filesystem::path source;
  std::string uri;
  std::optional<std::string> region;
  fetch_progress_cb_t progress{};
};

void aws_s3_upload(s3_upload_request const &request);

// Throws, saying which sources failed and why, when transfers would go unsigned. Call
// once before writes: unsigned ones fail per object, opaquely.
void aws_credentials_require(std::string_view op);

// Carries aws-c-* callbacks' throws across C frames: guard() keeps the first and runs
// on_throw (cancel); wait() rethrows it once finish(), the library's last call, arrives.
class c_completion : unmovable {
 public:
  template <typename F, typename G>
  void guard(F &&body, G &&on_throw) noexcept;
  void finish() noexcept;
  void wait();

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool done_{ false };
  std::exception_ptr error_;
};

template <typename F, typename G>
void c_completion::guard(F &&body, G &&on_throw) noexcept {
  try {
    body();
  } catch (...) {
    {
      std::lock_guard const lock{ mutex_ };
      if (!error_) { error_ = std::current_exception(); }
    }
    on_throw();
  }
}

class aws_shutdown_guard : unmovable {
 public:
  aws_shutdown_guard() = default;
  ~aws_shutdown_guard();
};

}  // namespace envy
