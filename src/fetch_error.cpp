#include "fetch_error.h"

namespace envy {

char const *fetch_error_kind_name(fetch_error_kind kind) {
  switch (kind) {
    case fetch_error_kind::CONNECT: return "connect";
    case fetch_error_kind::TRANSFER: return "transfer";
    case fetch_error_kind::TIMEOUT: return "timeout";
    case fetch_error_kind::HTTP_STATUS: return "http_status";
    case fetch_error_kind::ABORTED: return "aborted";
    case fetch_error_kind::PROTOCOL: return "protocol";
    case fetch_error_kind::LOCAL: return "local";
    case fetch_error_kind::OTHER: return "other";
  }
  return "other";
}

bool fetch_error_retryable(fetch_error const &err) {
  switch (err.kind()) {
    // The transport died on its own; the bytes we did get are discarded either way.
    case fetch_error_kind::CONNECT:
    case fetch_error_kind::TRANSFER:
    case fetch_error_kind::TIMEOUT: return true;

    // 5xx is the server admitting fault; 429 is it asking us to come back later.
    // Every other 4xx is a statement about the request, which a replay won't change.
    case fetch_error_kind::HTTP_STATUS:
      return err.http_status().value_or(0) >= 500 || err.http_status() == 429;

    case fetch_error_kind::ABORTED:
    case fetch_error_kind::PROTOCOL:
    case fetch_error_kind::LOCAL:
    case fetch_error_kind::OTHER: return false;
  }
  return false;
}

}  // namespace envy
