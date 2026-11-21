#include "aws_util.h"

#include "doctest.h"

#include <stdexcept>
#include <thread>

// --- aws_s3_parse_uri ---

TEST_CASE("aws_s3_parse_uri: bucket and key") {
  auto const parts{ envy::aws_s3_parse_uri("s3://my-bucket/releases/v1.2.3/envy.tar.gz",
                                           "test") };
  CHECK(parts.bucket == "my-bucket");
  CHECK(parts.key == "releases/v1.2.3/envy.tar.gz");
}

TEST_CASE("aws_s3_parse_uri: bucket only yields empty key") {
  // An upload target may name just a bucket; callers append their own prefix.
  auto const parts{ envy::aws_s3_parse_uri("s3://my-bucket", "test") };
  CHECK(parts.bucket == "my-bucket");
  CHECK(parts.key.empty());
}

TEST_CASE("aws_s3_parse_uri: bucket with trailing slash yields empty key") {
  auto const parts{ envy::aws_s3_parse_uri("s3://my-bucket/", "test") };
  CHECK(parts.bucket == "my-bucket");
  CHECK(parts.key.empty());
}

TEST_CASE("aws_s3_parse_uri: trailing slashes stripped from key") {
  // S3 keys are opaque bytes, so joining an unstripped "prefix/" with "/v1.2.3" would mint
  // "prefix//v1.2.3" -- a different object than the one readers ask for.
  CHECK(envy::aws_s3_parse_uri("s3://b/prefix/", "test").key == "prefix");
  CHECK(envy::aws_s3_parse_uri("s3://b/a/b/c///", "test").key == "a/b/c");
}

TEST_CASE("aws_s3_parse_uri: scheme match is case-insensitive") {
  // uri_classify accepts S3:// via istarts_with, so this must agree or a manifest mirror
  // classifies as S3 and then fails to parse.
  CHECK(envy::aws_s3_parse_uri("S3://b/k", "test").bucket == "b");
  CHECK(envy::aws_s3_parse_uri("s3://b/k", "test").bucket == "b");
  CHECK(envy::aws_s3_parse_uri("S3://B/K", "test").bucket == "B");
}

TEST_CASE("aws_s3_parse_uri: key case is preserved") {
  auto const parts{ envy::aws_s3_parse_uri("s3://Bucket/Mixed/Case/Key", "test") };
  CHECK(parts.bucket == "Bucket");
  CHECK(parts.key == "Mixed/Case/Key");
}

TEST_CASE("aws_s3_parse_uri: non-s3 scheme rejected") {
  CHECK_THROWS_AS(envy::aws_s3_parse_uri("https://example.com/x", "test"),
                  std::invalid_argument);
  CHECK_THROWS_AS(envy::aws_s3_parse_uri("/local/path", "test"), std::invalid_argument);
  CHECK_THROWS_AS(envy::aws_s3_parse_uri("", "test"), std::invalid_argument);
}

TEST_CASE("aws_s3_parse_uri: single-slash s3 typo rejected") {
  CHECK_THROWS_AS(envy::aws_s3_parse_uri("s3:/bucket/key", "test"), std::invalid_argument);
}

TEST_CASE("aws_s3_parse_uri: missing bucket rejected") {
  CHECK_THROWS_AS(envy::aws_s3_parse_uri("s3://", "test"), std::invalid_argument);
  CHECK_THROWS_AS(envy::aws_s3_parse_uri("s3:///key", "test"), std::invalid_argument);
}

TEST_CASE("aws_s3_parse_uri: op label appears in the error") {
  try {
    envy::aws_s3_parse_uri("nope", "mirror-envy");
    FAIL("expected throw");
  } catch (std::invalid_argument const &e) {
    CHECK(std::string{ e.what() }.starts_with("mirror-envy: "));
  }
}

// --- aws_s3_endpoint ---

namespace {

envy::s3_endpoint endpoint(std::string bucket,
                           std::string key,
                           std::string_view region = "us-west-2",
                           std::string_view override_url = {}) {
  return envy::aws_s3_endpoint({ .bucket = std::move(bucket), .key = std::move(key) },
                               region,
                               override_url);
}

}  // namespace

TEST_CASE("aws_s3_endpoint: AWS buckets are virtual-hosted in their region") {
  auto const e{ endpoint("my-bucket", "releases/v1/envy.tar.gz") };
  CHECK(e.scheme == "https");
  CHECK(e.authority == "my-bucket.s3.us-west-2.amazonaws.com");
  CHECK(e.path == "/releases/v1/envy.tar.gz");
}

TEST_CASE("aws_s3_endpoint: China regions use the amazonaws.com.cn partition") {
  CHECK(endpoint("b-1", "k", "cn-north-1").authority ==
        "b-1.s3.cn-north-1.amazonaws.com.cn");
}

TEST_CASE("aws_s3_endpoint: buckets that can't be a TLS host label go path-style") {
  // Dots break the *.s3 wildcard certificate; uppercase and underscores aren't DNS-safe.
  for (char const *bucket : { "my.bucket", "MyBucket", "my_bucket", "ab" }) {
    CAPTURE(bucket);
    auto const e{ endpoint(bucket, "k") };
    CHECK(e.authority == "s3.us-west-2.amazonaws.com");
    CHECK(e.path == "/" + std::string{ bucket } + "/k");
  }
}

TEST_CASE("aws_s3_endpoint: an IP or localhost override goes path-style on its port") {
  auto const ip{ endpoint("test-bucket", "a/b", "us-east-1", "http://127.0.0.1:9000") };
  CHECK(ip.scheme == "http");
  CHECK(ip.authority == "127.0.0.1:9000");
  CHECK(ip.path == "/test-bucket/a/b");

  auto const v6{ endpoint("test-bucket", "k", "us-east-1", "http://[::1]:9000/") };
  CHECK(v6.authority == "[::1]:9000");
  CHECK(v6.path == "/test-bucket/k");

  auto const local{ endpoint("test-bucket", "k", "us-east-1", "http://localhost:9000") };
  CHECK(local.authority == "localhost:9000");
  CHECK(local.path == "/test-bucket/k");
}

TEST_CASE("aws_s3_endpoint: a hostname override is virtual-hosted under its own path") {
  auto const e{ endpoint("bkt", "k", "auto", "https://acct.r2.example.com/gateway/") };
  CHECK(e.scheme == "https");
  CHECK(e.authority == "bkt.acct.r2.example.com");
  CHECK(e.path == "/gateway/k");
}

TEST_CASE("aws_s3_endpoint: keys are percent-encoded except for '/'") {
  CHECK(endpoint("bkt", "a b/c+d=é~_.-").path == "/a%20b/c%2Bd%3D%C3%A9~_.-");
}

// --- aws_s3_parse_error ---

TEST_CASE("aws_s3_parse_error: code and message, unescaped") {
  auto const e{ envy::aws_s3_parse_error(
      R"(<?xml version="1.0" encoding="UTF-8"?><Error><Code>AccessDenied</Code>)"
      R"(<Message>a &lt;b&gt; &amp; &quot;c&quot; &apos;d&apos;</Message></Error>)") };
  CHECK(e.code == "AccessDenied");
  CHECK(e.message == R"(a <b> & "c" 'd')");
}

TEST_CASE("aws_s3_parse_error: absent elements and empty bodies read as empty") {
  CHECK(envy::aws_s3_parse_error("<Error><Code>NoSuchKey</Code></Error>").message.empty());
  auto const none{ envy::aws_s3_parse_error("") };
  CHECK(none.code.empty());
  CHECK(none.message.empty());
}

// --- c_completion ---

TEST_CASE("c_completion: a callback's throw is rethrown on the waiting thread") {
  envy::c_completion done;
  bool cancelled{ false };
  std::thread library{ [&] {  // a C library's event-loop thread
    done.guard([] { throw std::runtime_error("from a callback"); },
               [&] { cancelled = true; });
    done.guard([] { throw std::logic_error("a later one"); }, [] {});
    done.finish();
  } };
  CHECK_THROWS_WITH_AS(done.wait(), "from a callback", std::runtime_error);
  library.join();
  CHECK(cancelled);
}

TEST_CASE("c_completion: wait returns once finished when nothing threw") {
  envy::c_completion done;
  int ran{ 0 };
  std::thread library{ [&] {
    done.guard([&] { ++ran; }, [] { FAIL("on_throw ran without a throw"); });
    done.finish();
  } };
  CHECK_NOTHROW(done.wait());
  library.join();
  CHECK(ran == 1);
}
