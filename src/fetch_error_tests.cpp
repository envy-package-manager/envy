#include "fetch_error.h"

#include "doctest.h"

namespace {

bool retryable(envy::fetch_error_kind kind, std::optional<int> status = std::nullopt) {
  return envy::fetch_error_retryable(envy::fetch_error{ kind, "test", status });
}

}  // namespace

TEST_CASE("transport failures retry") {
  // The transport died on its own; nothing about the request is known to be wrong.
  CHECK(retryable(envy::fetch_error_kind::CONNECT));
  CHECK(retryable(envy::fetch_error_kind::TRANSFER));
  CHECK(retryable(envy::fetch_error_kind::TIMEOUT));
}

TEST_CASE("local, protocol, and aborted failures never retry") {
  // A replay changes none of these: the URL stays malformed, the disk stays full,
  // and the user who cancelled still wants it cancelled.
  CHECK_FALSE(retryable(envy::fetch_error_kind::ABORTED));
  CHECK_FALSE(retryable(envy::fetch_error_kind::PROTOCOL));
  CHECK_FALSE(retryable(envy::fetch_error_kind::LOCAL));
  CHECK_FALSE(retryable(envy::fetch_error_kind::OTHER));
}

TEST_CASE("HTTP status decides its own retryability") {
  auto const http{ [](int status) {
    return retryable(envy::fetch_error_kind::HTTP_STATUS, status);
  } };

  CHECK(http(500));
  CHECK(http(502));
  CHECK(http(503));
  CHECK(http(504));
  CHECK(http(429));  // rate limited: the server is asking us to come back

  CHECK_FALSE(http(400));
  CHECK_FALSE(http(401));
  CHECK_FALSE(http(403));
  CHECK_FALSE(http(404));
  CHECK_FALSE(http(410));
  CHECK_FALSE(http(451));

  // A status-kind error with no code recorded is not evidence of a server fault.
  CHECK_FALSE(retryable(envy::fetch_error_kind::HTTP_STATUS));
}

TEST_CASE("fetch_error carries kind and status through the throw") {
  try {
    throw envy::fetch_error{ envy::fetch_error_kind::HTTP_STATUS, "HTTP error 503", 503 };
  } catch (envy::fetch_error const &e) {
    CHECK(e.kind() == envy::fetch_error_kind::HTTP_STATUS);
    REQUIRE(e.http_status().has_value());
    CHECK(*e.http_status() == 503);
    CHECK(std::string{ e.what() } == "HTTP error 503");
    CHECK(envy::fetch_error_retryable(e));
  }

  // Callers that only know std::exception still get the message.
  try {
    throw envy::fetch_error{ envy::fetch_error_kind::TIMEOUT, "stalled" };
  } catch (std::exception const &e) { CHECK(std::string{ e.what() } == "stalled"); }
}

TEST_CASE("every kind has a stable trace name") {
  using k = envy::fetch_error_kind;
  CHECK(std::string{ envy::fetch_error_kind_name(k::CONNECT) } == "connect");
  CHECK(std::string{ envy::fetch_error_kind_name(k::TRANSFER) } == "transfer");
  CHECK(std::string{ envy::fetch_error_kind_name(k::TIMEOUT) } == "timeout");
  CHECK(std::string{ envy::fetch_error_kind_name(k::HTTP_STATUS) } == "http_status");
  CHECK(std::string{ envy::fetch_error_kind_name(k::ABORTED) } == "aborted");
  CHECK(std::string{ envy::fetch_error_kind_name(k::PROTOCOL) } == "protocol");
  CHECK(std::string{ envy::fetch_error_kind_name(k::LOCAL) } == "local");
  CHECK(std::string{ envy::fetch_error_kind_name(k::OTHER) } == "other");
}
