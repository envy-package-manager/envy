#if !defined(_WIN32)

#include "fetch_http.h"

#include "fetch_error.h"

#include "curl/curl.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace envy {

namespace {

constexpr char kDefaultUserAgent[]{ "envy-fetch/0.0" };

void ensure_curl_initialized() {
  static std::once_flag once;
  std::call_once(once, [] {
    CURLcode const code{ curl_global_init(CURL_GLOBAL_DEFAULT) };
    if (code != CURLE_OK) {
      throw fetch_error(fetch_error_kind::LOCAL,
                        std::string("curl_global_init failed: ") +
                            curl_easy_strerror(code));
    }
  });
}

size_t curl_write_file(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *stream{ static_cast<std::ofstream *>(userdata) };
  size_t const total{ size * nmemb };
  stream->write(ptr, static_cast<std::streamsize>(total));
  if (!*stream) { return 0; }
  return total;
}

// Map curl's error space onto retry classes. Anything unrecognized is fatal: a retry
// costs a full re-download, so only failures known to be transient earn one.
fetch_error_kind classify_curl_error(CURLcode code) {
  switch (code) {
    case CURLE_OPERATION_TIMEDOUT: return fetch_error_kind::TIMEOUT;

    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_SSL_CONNECT_ERROR: return fetch_error_kind::CONNECT;

    case CURLE_PARTIAL_FILE:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM: return fetch_error_kind::TRANSFER;

    case CURLE_HTTP_RETURNED_ERROR: return fetch_error_kind::HTTP_STATUS;

    case CURLE_ABORTED_BY_CALLBACK: return fetch_error_kind::ABORTED;
    case CURLE_WRITE_ERROR: return fetch_error_kind::LOCAL;

    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_URL_MALFORMAT: return fetch_error_kind::PROTOCOL;

    default: return fetch_error_kind::OTHER;
  }
}

template <typename T>
std::optional<T> get_info(CURL *handle, CURLINFO info) {
  T value{};
  return curl_easy_getinfo(handle, info, &value) == CURLE_OK ? std::optional<T>{ value }
                                                             : std::nullopt;
}

// " from https://mirror.example/..." -- the post-redirect URL. SourceForge and friends
// hand out a per-request mirror, so the requested URL names nobody who can be blamed.
std::string from_effective_url(CURL *handle) {
  auto const url{ get_info<char *>(handle, CURLINFO_EFFECTIVE_URL) };
  return (url && *url && **url) ? " from " + std::string{ *url } : std::string{};
}

// " after 2201600 of 12600000 bytes" -- separates "never started" from "stalled at 17%".
std::string transfer_position(CURL *handle) {
  auto const downloaded{ get_info<curl_off_t>(handle, CURLINFO_SIZE_DOWNLOAD_T)
                             .value_or(0) };
  auto const announced{
    get_info<curl_off_t>(handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T).value_or(-1)
  };

  char buf[128];
  if (announced >= 0) {
    snprintf(buf,
             sizeof(buf),
             " after %lld of %lld bytes",
             static_cast<long long>(downloaded),
             static_cast<long long>(announced));
  } else {
    snprintf(buf,
             sizeof(buf),
             " after %lld bytes (length unknown)",
             static_cast<long long>(downloaded));
  }
  return buf;
}

[[noreturn]] void throw_curl_error(CURL *handle, CURLcode code) {
  auto const kind{ classify_curl_error(code) };

  // A status code explains itself, and the bytes counted would be the error page, not
  // the payload. Everything else needs to say how far the transfer got.
  if (kind == fetch_error_kind::HTTP_STATUS) {
    auto const status{ get_info<long>(handle, CURLINFO_RESPONSE_CODE).value_or(0) };
    throw fetch_error(kind,
                      "HTTP error " + std::to_string(status) +
                          from_effective_url(handle),
                      static_cast<int>(status));
  }

  throw fetch_error(kind,
                    curl_easy_strerror(code) + transfer_position(handle) +
                        from_effective_url(handle));
}

}  // namespace

std::filesystem::path fetch_http_download(std::string_view url,
                                          std::filesystem::path const &destination,
                                          fetch_progress_cb_t const &progress,
                                          std::optional<std::string> const &post_data) {
  ensure_curl_initialized();

  std::string const url_copy{ url };

  if (destination.empty()) {
    throw std::invalid_argument("fetch_http_download: destination is empty");
  }

  auto const resolved_destination{ [&] {
    auto p{ std::filesystem::path{ destination } };
    if (!p.is_absolute()) { p = std::filesystem::absolute(p); }
    return p.lexically_normal();
  }() };

  std::error_code ec;
  auto const parent{ resolved_destination.parent_path() };
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      throw fetch_error(fetch_error_kind::LOCAL,
                        "fetch_http_download: failed to create parent directory: " +
                            parent.string() + ": " + ec.message());
    }
  }

  std::ofstream output{ resolved_destination, std::ios::binary | std::ios::trunc };
  if (!output.is_open()) {
    throw fetch_error(fetch_error_kind::LOCAL,
                      "fetch_http_download: failed to open destination: " +
                          resolved_destination.string());
  }

  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle{ curl_easy_init(),
                                                              &curl_easy_cleanup };
  if (!handle) { throw fetch_error(fetch_error_kind::LOCAL, "curl_easy_init failed"); }

  auto const setopt{ [handle = handle.get()](auto option, auto value) {
    if (CURLcode const rc{ curl_easy_setopt(handle, option, value) }; rc != CURLE_OK) {
      throw fetch_error(fetch_error_kind::LOCAL,
                        std::string("curl_easy_setopt failed: ") +
                            curl_easy_strerror(rc));
    }
  } };

  // One cleanup path for every transport failure: no caller -- retry included -- should
  // ever find a truncated payload sitting at the destination.
  try {
    setopt(CURLOPT_URL, url_copy.c_str());
    setopt(CURLOPT_FOLLOWLOCATION, 1L);
    setopt(CURLOPT_FAILONERROR, 1L);
    setopt(CURLOPT_NOSIGNAL, 1L);
    setopt(CURLOPT_CONNECTTIMEOUT, 30L);
    setopt(CURLOPT_LOW_SPEED_LIMIT, 1L);
    setopt(CURLOPT_LOW_SPEED_TIME, 60L);
    setopt(CURLOPT_USERAGENT, kDefaultUserAgent);
    setopt(CURLOPT_WRITEFUNCTION, curl_write_file);
    setopt(CURLOPT_WRITEDATA, &output);
    setopt(CURLOPT_NOPROGRESS, progress ? 0L : 1L);

    if (post_data) {
      setopt(CURLOPT_POST, 1L);
      setopt(CURLOPT_POSTFIELDS, post_data->c_str());
      setopt(CURLOPT_POSTFIELDSIZE, static_cast<long>(post_data->size()));
    }

    if (progress) {
      setopt(
          CURLOPT_XFERINFOFUNCTION,
          +[](void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t)
              -> int {
            return (*static_cast<fetch_progress_cb_t const *>(clientp))(fetch_progress_t{
                       std::in_place_type<fetch_transfer_progress>,
                       fetch_transfer_progress{
                           .transferred = static_cast<std::uint64_t>(dlnow),
                           .total = dltotal ? std::optional<std::uint64_t>{ static_cast<
                                                  std::uint64_t>(dltotal) }
                                            : std::nullopt } })
                       ? 0
                       : 1;
          });

      setopt(CURLOPT_XFERINFODATA, &progress);
    }

    if (CURLcode const perform_result{ curl_easy_perform(handle.get()) };
        perform_result != CURLE_OK) {
      throw_curl_error(handle.get(), perform_result);
    }

    output.flush();
    if (!output) {
      throw fetch_error(fetch_error_kind::LOCAL,
                        "fetch_http_download: failed to flush destination file");
    }
    output.close();
  } catch (...) {
    output.close();
    std::error_code cleanup_ec;
    std::filesystem::remove(resolved_destination, cleanup_ec);
    throw;
  }

  return resolved_destination;
}

}  // namespace envy

#endif  // !defined(_WIN32)
