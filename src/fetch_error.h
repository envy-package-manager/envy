#pragma once

#include <optional>
#include <stdexcept>
#include <string>

namespace envy {

// Transport failure classification. Retryability is a property of the kind (and, for
// HTTP_STATUS, the status code) so callers never have to string-match a message.
enum class fetch_error_kind {
  CONNECT,      // DNS, TCP connect, or TLS handshake failed before any body arrived
  TRANSFER,     // connection died mid-body
  TIMEOUT,      // stalled below the minimum transfer rate, or a timeout elapsed
  HTTP_STATUS,  // server answered with a >= 400 status
  ABORTED,      // progress callback asked to stop
  PROTOCOL,     // malformed URL or unsupported scheme
  LOCAL,        // local filesystem or library-init failure
  OTHER,        // unclassified; treated as fatal
};

char const *fetch_error_kind_name(fetch_error_kind kind);

class fetch_error : public std::runtime_error {
 public:
  fetch_error(fetch_error_kind kind,
              std::string const &what,
              std::optional<int> http_status = std::nullopt)
      : std::runtime_error(what), kind_{ kind }, http_status_{ http_status } {}

  fetch_error_kind kind() const { return kind_; }
  std::optional<int> http_status() const { return http_status_; }

 private:
  fetch_error_kind kind_;
  std::optional<int> http_status_;  // set only when kind_ == HTTP_STATUS
};

// Retry is safe for everything envy fetches: transfers are idempotent GETs (the lone
// non-GET is a license click-through POST, which replays cleanly) and every payload is
// sha256-verified after transport, so a retry cannot launder bad bytes.
bool fetch_error_retryable(fetch_error const &err);

}  // namespace envy
