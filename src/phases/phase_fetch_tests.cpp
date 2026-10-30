#include "phase_fetch.h"

#include "sol/sol.hpp"

#include "doctest.h"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

namespace envy {

TEST_CASE("url_to_fetch_request HTTP") {
  auto req{ url_to_fetch_request({ .source = "http://example.com/file.tar.gz" }, "/tmp/file.tar.gz", "test") };

  REQUIRE(std::holds_alternative<fetch_request_http>(req));
  auto const &http_req{ std::get<fetch_request_http>(req) };
  CHECK(http_req.source == "http://example.com/file.tar.gz");
  CHECK(http_req.destination == "/tmp/file.tar.gz");
  CHECK_FALSE(http_req.post_data.has_value());
}

TEST_CASE("url_to_fetch_request HTTPS") {
  auto req{ url_to_fetch_request({ .source = "https://example.com/file.tar.gz" }, "/tmp/file.tar.gz", "test") };

  REQUIRE(std::holds_alternative<fetch_request_https>(req));
  auto const &https_req{ std::get<fetch_request_https>(req) };
  CHECK(https_req.source == "https://example.com/file.tar.gz");
  CHECK(https_req.destination == "/tmp/file.tar.gz");
  CHECK_FALSE(https_req.post_data.has_value());
}

TEST_CASE("url_to_fetch_request FTP") {
  auto req{ url_to_fetch_request({ .source = "ftp://example.com/file.tar.gz" }, "/tmp/file.tar.gz", "test") };

  REQUIRE(std::holds_alternative<fetch_request_ftp>(req));
  auto const &ftp_req{ std::get<fetch_request_ftp>(req) };
  CHECK(ftp_req.source == "ftp://example.com/file.tar.gz");
  CHECK(ftp_req.destination == "/tmp/file.tar.gz");
}

TEST_CASE("url_to_fetch_request FTPS") {
  auto req{ url_to_fetch_request({ .source = "ftps://example.com/file.tar.gz" }, "/tmp/file.tar.gz", "test") };

  REQUIRE(std::holds_alternative<fetch_request_ftps>(req));
  auto const &ftps_req{ std::get<fetch_request_ftps>(req) };
  CHECK(ftps_req.source == "ftps://example.com/file.tar.gz");
  CHECK(ftps_req.destination == "/tmp/file.tar.gz");
}

TEST_CASE("url_to_fetch_request S3") {
  auto req{ url_to_fetch_request({ .source = "s3://bucket/file.tar.gz" }, "/tmp/file.tar.gz", "test") };

  REQUIRE(std::holds_alternative<fetch_request_s3>(req));
  auto const &s3_req{ std::get<fetch_request_s3>(req) };
  CHECK(s3_req.source == "s3://bucket/file.tar.gz");
  CHECK(s3_req.destination == "/tmp/file.tar.gz");
}

TEST_CASE("url_to_fetch_request file absolute") {
  auto req{ url_to_fetch_request({ .source = "/absolute/path/file.tar.gz" }, "/tmp/file.tar.gz", "test") };

  REQUIRE(std::holds_alternative<fetch_request_file>(req));
  auto const &file_req{ std::get<fetch_request_file>(req) };
  CHECK(file_req.source == "/absolute/path/file.tar.gz");
  CHECK(file_req.destination == "/tmp/file.tar.gz");
}

TEST_CASE("url_to_fetch_request file relative") {
  auto req{ url_to_fetch_request({ .source = "relative/path/file.tar.gz" }, "/tmp/file.tar.gz", "test") };

  REQUIRE(std::holds_alternative<fetch_request_file>(req));
  auto const &file_req{ std::get<fetch_request_file>(req) };
  CHECK(file_req.source == "relative/path/file.tar.gz");
  CHECK(file_req.destination == "/tmp/file.tar.gz");
}

TEST_CASE("url_to_fetch_request git with ref") {
  auto req{ url_to_fetch_request({ .source = "git://github.com/user/repo.git", .ref = "abc123def456" }, "/tmp/repo", "test") };

  REQUIRE(std::holds_alternative<fetch_request_git>(req));
  auto const &git_req{ std::get<fetch_request_git>(req) };
  CHECK(git_req.source == "git://github.com/user/repo.git");
  CHECK(git_req.destination == "/tmp/repo");
  CHECK(git_req.ref == "abc123def456");
  CHECK(git_req.scheme == uri_scheme::GIT);
}

TEST_CASE("url_to_fetch_request git_https with ref") {
  auto req{ url_to_fetch_request({ .source = "https://github.com/user/repo.git", .ref = "main" }, "/tmp/repo", "test") };

  REQUIRE(std::holds_alternative<fetch_request_git>(req));
  auto const &git_req{ std::get<fetch_request_git>(req) };
  CHECK(git_req.source == "https://github.com/user/repo.git");
  CHECK(git_req.destination == "/tmp/repo");
  CHECK(git_req.ref == "main");
  CHECK(git_req.scheme == uri_scheme::GIT_HTTPS);
}

TEST_CASE("url_to_fetch_request git without ref throws") {
  CHECK_THROWS_WITH_AS(url_to_fetch_request({ .source = "git://github.com/user/repo.git" }, "/tmp/repo", "test"),
                       "Git URLs require 'ref' field in test",
                       std::runtime_error);
}

TEST_CASE("url_to_fetch_request git with empty ref throws") {
  CHECK_THROWS_WITH_AS(url_to_fetch_request({ .source = "git://github.com/user/repo.git", .ref = "" }, "/tmp/repo", "test"),
                       "Git URLs require 'ref' field in test",
                       std::runtime_error);
}

TEST_CASE("url_to_fetch_request unsupported scheme throws") {
  CHECK_THROWS_WITH_AS(
      url_to_fetch_request({ .source = "unsupported://example.com/file" }, "/tmp/file", "test context"),
      "Unsupported URL scheme in test context: unsupported://example.com/file",
      std::runtime_error);
}

TEST_CASE("url_to_fetch_request HTTP with post_data") {
  auto req{ url_to_fetch_request({ .source = "http://example.com/download", .post_data = "key=value" }, "/tmp/file", "test") };

  REQUIRE(std::holds_alternative<fetch_request_http>(req));
  auto const &http_req{ std::get<fetch_request_http>(req) };
  REQUIRE(http_req.post_data.has_value());
  CHECK(http_req.post_data.value() == "key=value");
}

TEST_CASE("url_to_fetch_request HTTPS with post_data") {
  auto req{ url_to_fetch_request({ .source = "https://example.com/download", .post_data = "accept_license=yes" }, "/tmp/file", "test") };

  REQUIRE(std::holds_alternative<fetch_request_https>(req));
  auto const &https_req{ std::get<fetch_request_https>(req) };
  REQUIRE(https_req.post_data.has_value());
  CHECK(https_req.post_data.value() == "accept_license=yes");
}

TEST_CASE("url_to_fetch_request post_data on FTP throws") {
  CHECK_THROWS_WITH_AS(url_to_fetch_request({ .source = "ftp://example.com/file", .post_data = "data=value" }, "/tmp/file", "test"),
                       "post_data only valid for HTTP/HTTPS in test",
                       std::runtime_error);
}

TEST_CASE("url_to_fetch_request post_data on git throws") {
  CHECK_THROWS_WITH_AS(url_to_fetch_request({ .source = "git://github.com/user/repo.git", .ref = "main", .post_data = "data=value" }, "/tmp/repo", "test"),
                       "post_data only valid for HTTP/HTTPS in test",
                       std::runtime_error);
}

TEST_CASE("url_to_fetch_request post_data on file throws") {
  CHECK_THROWS_WITH_AS(url_to_fetch_request({ .source = "/path/to/file", .post_data = "data=value" }, "/tmp/file", "test"),
                       "post_data only valid for HTTP/HTTPS in test",
                       std::runtime_error);
}

TEST_CASE("url_to_fetch_request file with file_root") {
  auto req{ url_to_fetch_request({ .source = "relative/path/file.tar.gz" }, "/tmp/file.tar.gz", "test", "/some/root") };

  REQUIRE(std::holds_alternative<fetch_request_file>(req));
  auto const &file_req{ std::get<fetch_request_file>(req) };
  CHECK(file_req.source == "relative/path/file.tar.gz");
  CHECK(file_req.destination == "/tmp/file.tar.gz");
  CHECK(file_req.file_root == "/some/root");
}

TEST_CASE("url_to_fetch_request file without file_root") {
  auto req{ url_to_fetch_request({ .source = "relative/path/file.tar.gz" }, "/tmp/file.tar.gz", "test") };

  REQUIRE(std::holds_alternative<fetch_request_file>(req));
  auto const &file_req{ std::get<fetch_request_file>(req) };
  CHECK(file_req.file_root.empty());
}

TEST_CASE("url_to_fetch_request git uses canonical source") {
  auto req{ url_to_fetch_request({ .source = "https://github.com/user/repo.git", .ref = "main" }, "/tmp/repo", "test") };

  REQUIRE(std::holds_alternative<fetch_request_git>(req));
  auto const &git_req{ std::get<fetch_request_git>(req) };
  // Should use canonical form from uri_classify, not raw URL
  CHECK_FALSE(git_req.source.empty());
}

TEST_CASE("url_to_fetch_request git carries submodules") {
  auto req{ url_to_fetch_request(
      { .source = "https://github.com/user/repo.git",
        .ref = "main",
        .submodules = glob_filter{ .include = {}, .exclude = { "a" } } },
      "/tmp/repo",
      "test") };

  auto const &git_req{ std::get<fetch_request_git>(req) };
  REQUIRE(git_req.submodules.has_value());
  CHECK(git_req.submodules->exclude == std::vector<std::string>{ "a" });
}

TEST_CASE("url_to_fetch_request submodules on a non-git source throws") {
  CHECK_THROWS_WITH_AS(
      url_to_fetch_request({ .source = "https://example.com/a.tar.gz",
                             .submodules = glob_filter{} },
                           "/tmp/a",
                           "test"),
      "submodules requires a git source in test: https://example.com/a.tar.gz",
      std::runtime_error);
}

TEST_CASE("fetch_entry_parse reads every key and rejects the rest") {
  sol::state lua;
  auto table{ [&](char const *script) {
    lua.script(std::string{ "t = " } + script);
    return sol::table{ lua["t"] };
  } };

  auto const entry{ fetch_entry_parse(
      table("{ source = 'https://h/r.git', ref = 'main', sha256 = 'ab', dest = 'r', "
            "post_data = 'k=v', submodules = { '!x' } }"),
      "test") };
  CHECK(entry.source == "https://h/r.git");
  CHECK(entry.ref == "main");
  CHECK(entry.sha256 == "ab");
  CHECK(entry.dest == "r");
  CHECK(entry.post_data == "k=v");
  REQUIRE(entry.submodules.has_value());
  CHECK(entry.submodules->exclude == std::vector<std::string>{ "x" });

  CHECK_THROWS_WITH(
      fetch_entry_parse(table("{ source = 'https://h/r.git', submodule = true }"), "test"),
      doctest::Contains("unknown key 'submodule'"));
  CHECK_THROWS_WITH(fetch_entry_parse(table("{ ref = 'main' }"), "test"),
                    doctest::Contains("missing 'source'"));
}

}  // namespace envy
