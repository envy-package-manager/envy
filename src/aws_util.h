#pragma once

#include "fetch.h"
#include "util.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace envy {

void aws_init();
void aws_shutdown();

struct s3_uri_parts {
  std::string bucket;
  std::string key;  // empty when the URI names only a bucket
};

// Splits s3://bucket[/key]. The scheme match is case-insensitive because uri_classify
// accepts S3:// too. A trailing '/' is stripped from the key so callers can join with '/'
// without minting a distinct double-slash key -- S3 keys are opaque bytes, so "a//b" and
// "a/b" are different objects. op appears in error messages only.
s3_uri_parts aws_s3_parse_uri(std::string_view uri, std::string_view op);

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

// Resolves credentials as a transfer would, throwing if none are usable and quoting the
// SDK's account of why. Call once before writes: unsigned ones fail per object, opaquely.
void aws_credentials_require(std::optional<std::string> const &region,
                             std::string_view op);

class aws_shutdown_guard : unmovable {
 public:
  aws_shutdown_guard() = default;
  ~aws_shutdown_guard();
};

}  // namespace envy
