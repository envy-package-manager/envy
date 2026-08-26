#if defined(_WIN32)

#include "fetch_http.h"

#include "fetch_error.h"
#include "uri.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <Windows.h>
#include <WinInet.h>
// clang-format on

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace envy {

namespace {

constexpr char kDefaultUserAgent[]{ "envy-fetch/0.0" };
constexpr DWORD kReadBufferSize{ 65536 };
constexpr DWORD kCommonFlags{ INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                              INTERNET_FLAG_KEEP_CONNECTION };

// WinINet has no low-speed-limit equivalent, so a per-operation deadline is the closest
// bound on the curl backend's CONNECTTIMEOUT 30 / LOW_SPEED_LIMIT 1 + LOW_SPEED_TIME 60.
// Without these a mirror that accepts the connection and then stops sending wedges the
// worker thread forever.
constexpr DWORD kConnectTimeoutMs{ 30000 };
constexpr DWORD kTransferTimeoutMs{ 60000 };

// Best-effort: WinINet rejects some options per handle class (connect timeout is not
// valid on a request handle), and a download that runs unbounded still beats one that
// refuses to start.
void set_option_dword(HINTERNET handle, DWORD option, DWORD value) {
  InternetSetOptionA(handle, option, &value, sizeof(value));
}

std::string win_error_message(DWORD error_code) {
  char *buf{ nullptr };

  // Try wininet.dll first for WinINet-specific error messages, then system.
  DWORD len{ FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                FORMAT_MESSAGE_FROM_HMODULE |
                                FORMAT_MESSAGE_IGNORE_INSERTS,
                            GetModuleHandleA("wininet.dll"),
                            error_code,
                            0,
                            reinterpret_cast<LPSTR>(&buf),
                            0,
                            nullptr) };
  if (!len || !buf) {
    len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS,
                         nullptr,
                         error_code,
                         0,
                         reinterpret_cast<LPSTR>(&buf),
                         0,
                         nullptr);
  }
  if (!len || !buf) {
    if (buf) { LocalFree(buf); }
    char code_buf[64];
    snprintf(code_buf,
             sizeof(code_buf),
             "error code %lu",
             static_cast<unsigned long>(error_code));
    return code_buf;
  }

  // Strip trailing \r\n from FormatMessage output.
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) { --len; }
  std::string const msg{ buf, len };
  LocalFree(buf);
  return msg;
}

std::string wininet_extended_error_info() {
  DWORD error_code{ 0 };
  DWORD buf_len{ 0 };
  InternetGetLastResponseInfoA(&error_code, nullptr, &buf_len);
  if (buf_len == 0) { return {}; }
  ++buf_len;  // account for null terminator
  std::string buf(buf_len, '\0');
  if (!InternetGetLastResponseInfoA(&error_code, buf.data(), &buf_len)) { return {}; }
  buf.resize(buf_len);
  while (!buf.empty() && (buf.back() == '\n' || buf.back() == '\r')) { buf.pop_back(); }
  return buf;
}

// Map WinINet's error space onto retry classes. `fallback` carries the call site's own
// knowledge (a failed read is a transfer failure even when the code is unrecognized).
fetch_error_kind classify_wininet_error(DWORD err, fetch_error_kind fallback) {
  switch (err) {
    case ERROR_INTERNET_TIMEOUT: return fetch_error_kind::TIMEOUT;

    case ERROR_INTERNET_NAME_NOT_RESOLVED:
    case ERROR_INTERNET_CANNOT_CONNECT:
    case ERROR_INTERNET_SERVER_UNREACHABLE:
    case ERROR_INTERNET_PROXY_SERVER_UNREACHABLE: return fetch_error_kind::CONNECT;

    case ERROR_INTERNET_CONNECTION_ABORTED:
    case ERROR_INTERNET_CONNECTION_RESET: return fetch_error_kind::TRANSFER;

    case ERROR_INTERNET_OPERATION_CANCELLED: return fetch_error_kind::ABORTED;

    default: return fallback;
  }
}

[[noreturn]] void throw_wininet_error(std::string const &context, fetch_error_kind kind) {
  DWORD const err{ GetLastError() };

  // GetLastError() == 0 means the Win32 error code was not set, which produces
  // the unhelpful "The operation completed successfully".  Try to recover
  // something useful from WinINet's per-thread extended error buffer or,
  // failing that, say plainly that the transport died without explaining itself.
  if (err == 0) {
    if (auto const extended{ wininet_extended_error_info() }; !extended.empty()) {
      throw fetch_error(kind, context + ": " + extended);
    }
    throw fetch_error(kind, context + ": connection closed without an error code");
  }

  // For WinINet-specific errors, append extended server response info when
  // available (e.g. FTP server replies, HTTP auth challenge text).
  std::string const msg{ [&] {
    std::ostringstream m;
    m << context << ": " << win_error_message(err);
    if (err == ERROR_INTERNET_EXTENDED_ERROR) {
      if (auto const extended{ wininet_extended_error_info() }; !extended.empty()) {
        m << " (" << extended << ")";
      }
    }
    return m.str();
  }() };

  throw fetch_error(classify_wininet_error(err, kind), msg);
}

// Process-wide WinInet session.  Created once on first download; Windows
// reclaims the handle at process exit.  Sharing a single session avoids
// repeated proxy auto-detection (WPAD) that serializes concurrent downloads.
std::once_flag g_session_once;
HINTERNET g_session{ nullptr };

HINTERNET ensure_session() {
  std::call_once(g_session_once, [] {
    g_session = InternetOpenA(kDefaultUserAgent,
                              INTERNET_OPEN_TYPE_PRECONFIG,
                              nullptr,
                              nullptr,
                              0);
    if (!g_session) {
      throw_wininet_error("InternetOpen failed", fetch_error_kind::LOCAL);
    }

    // Handles derived from the session inherit these, which is the only way to bound
    // InternetOpenUrl -- it connects and reads before handing back a request handle.
    set_option_dword(g_session, INTERNET_OPTION_CONNECT_TIMEOUT, kConnectTimeoutMs);
    set_option_dword(g_session, INTERNET_OPTION_SEND_TIMEOUT, kTransferTimeoutMs);
    set_option_dword(g_session, INTERNET_OPTION_RECEIVE_TIMEOUT, kTransferTimeoutMs);
  });
  return g_session;
}

struct internet_handle_deleter {
  void operator()(HINTERNET h) const {
    if (h) { InternetCloseHandle(h); }
  }
};
using internet_handle = std::unique_ptr<void, internet_handle_deleter>;

// A WinINet query that *succeeds* clears the thread's last-error along the way, so a
// failure diagnosed after one reads back as "the operation completed successfully" --
// the very "GetLastError returned 0" report this file exists to stop emitting. Every
// handle-inspection helper below puts the code back before returning.
struct last_error_guard {
  DWORD const err{ GetLastError() };
  ~last_error_guard() { SetLastError(err); }
};

std::optional<std::uint64_t> query_content_length(HINTERNET request) {
  last_error_guard const preserve_error{};

  char buf[32]{};
  DWORD buf_len{ sizeof(buf) };
  DWORD header_index{ 0 };
  if (HttpQueryInfoA(request, HTTP_QUERY_CONTENT_LENGTH, buf, &buf_len, &header_index)) {
    char *end{ nullptr };
    auto const val{ std::strtoull(buf, &end, 10) };
    if (end != buf && *end == '\0') { return val; }
  }
  return std::nullopt;
}

// The post-redirect URL. SourceForge and friends hand out a per-request mirror, so the
// requested URL names nobody who can be blamed for the failure.
std::string query_effective_url(HINTERNET request) {
  last_error_guard const preserve_error{};

  char buf[2048]{};
  DWORD len{ sizeof(buf) - 1 };
  return InternetQueryOptionA(request, INTERNET_OPTION_URL, buf, &len) ? std::string{ buf }
                                                                       : std::string{};
}

// " after 2201600 of 12600000 bytes from https://mirror.example/..." -- the byte count
// separates "never started" from "stalled at 17%", and the host names the mirror.
std::string transfer_position(HINTERNET request,
                              std::uint64_t transferred,
                              std::optional<std::uint64_t> content_length) {
  char buf[128];
  if (content_length) {
    snprintf(buf,
             sizeof(buf),
             " after %llu of %llu bytes",
             static_cast<unsigned long long>(transferred),
             static_cast<unsigned long long>(*content_length));
  } else {
    snprintf(buf,
             sizeof(buf),
             " after %llu bytes (length unknown)",
             static_cast<unsigned long long>(transferred));
  }

  std::string msg{ buf };
  if (auto const url{ query_effective_url(request) }; !url.empty()) {
    msg += " from " + url;
  }
  return msg;
}

void check_http_status(HINTERNET request) {
  DWORD status_code{ 0 };
  DWORD size{ sizeof(status_code) };
  DWORD header_index{ 0 };
  if (!HttpQueryInfoA(request,
                      HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                      &status_code,
                      &size,
                      &header_index)) {
    return;
  }
  if (status_code < 400) { return; }

  char msg[128];
  snprintf(msg, sizeof(msg), "HTTP error %lu", static_cast<unsigned long>(status_code));

  std::string full{ msg };
  if (auto const url{ query_effective_url(request) }; !url.empty()) {
    full += " from " + url;
  }
  throw fetch_error(fetch_error_kind::HTTP_STATUS, full, static_cast<int>(status_code));
}

void read_response_to_file(HINTERNET request,
                           std::ofstream &output,
                           fetch_progress_cb_t const &progress,
                           std::optional<std::uint64_t> content_length) {
  char buffer[kReadBufferSize];
  std::uint64_t bytes_read_total{ 0 };

  for (;;) {
    DWORD bytes_read{ 0 };
    if (!InternetReadFile(request, buffer, sizeof(buffer), &bytes_read)) {
      throw_wininet_error(
          "read failed" + transfer_position(request, bytes_read_total, content_length),
          fetch_error_kind::TRANSFER);
    }
    if (bytes_read == 0) { break; }

    output.write(buffer, static_cast<std::streamsize>(bytes_read));
    if (!output) {
      throw fetch_error(fetch_error_kind::LOCAL,
                        "fetch_http_download: failed to write to destination file");
    }

    bytes_read_total += bytes_read;

    if (progress) {
      bool const should_continue{ progress(
          fetch_progress_t{ std::in_place_type<fetch_transfer_progress>,
                            fetch_transfer_progress{ .transferred = bytes_read_total,
                                                     .total = content_length } }) };
      if (!should_continue) {
        throw fetch_error(fetch_error_kind::ABORTED,
                          "fetch_http_download: transfer aborted by progress callback");
      }
    }
  }

  // A server that announced a length and then hung up early leaves a short file whose
  // sha256 would fail anyway; naming the shortfall is a retryable transfer failure.
  if (content_length && bytes_read_total < *content_length) {
    throw fetch_error(fetch_error_kind::TRANSFER,
                      "connection closed early" +
                          transfer_position(request, bytes_read_total, content_length));
  }
}

void flush_output(std::ofstream &output) {
  output.flush();
  if (!output) {
    throw fetch_error(fetch_error_kind::LOCAL,
                      "fetch_http_download: failed to flush destination file");
  }
  output.close();
}

void download_with_post(std::string_view url,
                        std::ofstream &output,
                        fetch_progress_cb_t const &progress,
                        std::string const &post_body,
                        HINTERNET session) {
  // Parse URL components for InternetConnect + HttpOpenRequest
  URL_COMPONENTSA uc{};
  uc.dwStructSize = sizeof(uc);
  char host[256]{};
  char path[2048]{};
  uc.lpszHostName = host;
  uc.dwHostNameLength = sizeof(host);
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = sizeof(path);

  std::string const url_str{ url };
  if (!InternetCrackUrlA(url_str.c_str(), static_cast<DWORD>(url_str.size()), 0, &uc)) {
    throw_wininet_error("InternetCrackUrl failed (URL may exceed buffer capacity)",
                        fetch_error_kind::PROTOCOL);
  }

  DWORD const flags{ kCommonFlags |
                     (uc.nScheme == INTERNET_SCHEME_HTTPS ? INTERNET_FLAG_SECURE : 0) };

  internet_handle const connection{ InternetConnectA(session,
                                                     host,
                                                     uc.nPort,
                                                     nullptr,
                                                     nullptr,
                                                     INTERNET_SERVICE_HTTP,
                                                     0,
                                                     0) };
  if (!connection) {
    throw_wininet_error("InternetConnect failed", fetch_error_kind::CONNECT);
  }
  set_option_dword(connection.get(), INTERNET_OPTION_CONNECT_TIMEOUT, kConnectTimeoutMs);

  internet_handle const request{
    HttpOpenRequestA(connection.get(), "POST", path, nullptr, nullptr, nullptr, flags, 0)
  };
  if (!request) {
    throw_wininet_error("HttpOpenRequest failed", fetch_error_kind::CONNECT);
  }
  set_option_dword(request.get(), INTERNET_OPTION_SEND_TIMEOUT, kTransferTimeoutMs);
  set_option_dword(request.get(), INTERNET_OPTION_RECEIVE_TIMEOUT, kTransferTimeoutMs);

  // Kick the TUI before the blocking send so the user sees immediate progress.
  if (progress) {
    progress(fetch_progress_t{
        std::in_place_type<fetch_transfer_progress>,
        fetch_transfer_progress{ .transferred = 0, .total = std::nullopt } });
  }

  char const *content_type{ "Content-Type: application/x-www-form-urlencoded\r\n" };
  if (!HttpSendRequestA(
          request.get(),
          content_type,
          static_cast<DWORD>(strlen(content_type)),
          const_cast<char *>(post_body.c_str()),  // NOLINT: HttpSendRequestA takes
                                                  // non-const LPVOID but doesn't modify it
          static_cast<DWORD>(post_body.size()))) {
    throw_wininet_error("HttpSendRequest failed", fetch_error_kind::CONNECT);
  }

  check_http_status(request.get());
  read_response_to_file(request.get(),
                        output,
                        progress,
                        query_content_length(request.get()));
  flush_output(output);
}

void download_with_get(std::string_view url,
                       std::ofstream &output,
                       fetch_progress_cb_t const &progress,
                       HINTERNET session) {
  // Kick the TUI immediately so the user sees progress before the blocking
  // DNS + TLS handshake inside InternetOpenUrlA.
  if (progress) {
    progress(fetch_progress_t{
        std::in_place_type<fetch_transfer_progress>,
        fetch_transfer_progress{ .transferred = 0, .total = std::nullopt } });
  }

  std::string const url_str{ url };
  DWORD const flags{ kCommonFlags |
                     (uri_is_https_scheme(url) ? INTERNET_FLAG_SECURE : 0) };

  // InternetOpenUrl handles redirects automatically; it serves FTP as well as HTTP(S).
  internet_handle const request{
    InternetOpenUrlA(session, url_str.c_str(), nullptr, 0, flags, 0)
  };
  if (!request) {
    throw_wininet_error("InternetOpenUrl failed", fetch_error_kind::CONNECT);
  }
  set_option_dword(request.get(), INTERNET_OPTION_RECEIVE_TIMEOUT, kTransferTimeoutMs);

  // Check HTTP status for HTTP(S) URLs; FTP doesn't have HTTP status codes
  bool const is_http{ uri_is_http_scheme(url) };
  if (is_http) { check_http_status(request.get()); }

  read_response_to_file(request.get(),
                        output,
                        progress,
                        is_http ? query_content_length(request.get()) : std::nullopt);
  flush_output(output);
}

}  // namespace

std::filesystem::path fetch_http_download(std::string_view url,
                                          std::filesystem::path const &destination,
                                          fetch_progress_cb_t const &progress,
                                          std::optional<std::string> const &post_data) {
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

  HINTERNET const session{ ensure_session() };

  // One cleanup path for every transport failure: no caller -- retry included -- should
  // ever find a truncated payload sitting at the destination.
  try {
    // POST requires InternetConnect + HttpOpenRequest + HttpSendRequest
    if (post_data && uri_is_http_scheme(url)) {
      download_with_post(url, output, progress, *post_data, session);
    } else {
      download_with_get(url, output, progress, session);
    }
  } catch (...) {
    output.close();
    std::error_code cleanup_ec;
    std::filesystem::remove(resolved_destination, cleanup_ec);
    throw;
  }

  return resolved_destination;
}

}  // namespace envy

#endif  // defined(_WIN32)
