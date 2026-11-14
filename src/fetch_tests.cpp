#include "fetch.h"

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <variant>

TEST_CASE("fetch rejects empty sources") {
  envy::fetch_request_file request{ .source = "",
                                    .destination = std::filesystem::path("ignored"),
                                    .file_root = {} };

  auto const results{ envy::fetch({ request }) };
  REQUIRE(results.size() == 1);
  REQUIRE(std::holds_alternative<std::string>(results[0]));
  CHECK(std::get<std::string>(results[0]).find("empty") != std::string::npos);
}

TEST_CASE("fetch rejects unsupported schemes without retrying") {
  // No retry makes a scheme supported; WinINet's code for one once fell through to CONNECT
  // and spent the whole 5s connect budget before reporting it.
  bool retried{ false };  // written on fetch's worker thread, read after fetch joins it
  envy::fetch_request_http request{
    .source = "foo://bucket/object",
    .destination = std::filesystem::path("ignored"),
    .progress =
        [&](envy::fetch_progress_t const &p) {
          retried = retried || std::holds_alternative<envy::fetch_retry_progress>(p);
          return true;
        }
  };

  auto const results{ envy::fetch({ request }) };
  REQUIRE(results.size() == 1);
  REQUIRE(std::holds_alternative<std::string>(results[0]));
  // The error message should mention an error occurred
  auto const &error = std::get<std::string>(results[0]);
  INFO("Actual error message: ", error);
  CHECK(!error.empty());
  CHECK_FALSE(retried);
}

TEST_CASE("fetch batch download multiple files") {
  auto const temp_dir = std::filesystem::temp_directory_path() / "fetch_test_batch";
  std::filesystem::create_directories(temp_dir);

  // Create source files
  auto const source1 = temp_dir / "source1.txt";
  auto const source2 = temp_dir / "source2.txt";
  auto const source3 = temp_dir / "source3.txt";

  std::ofstream(source1) << "content one";
  std::ofstream(source2) << "content two";
  std::ofstream(source3) << "content three";

  // Create destination directory
  auto const dest_dir = temp_dir / "dest";
  std::filesystem::create_directories(dest_dir);

  // Batch fetch all three files
  std::vector<envy::fetch_request> requests{
    envy::fetch_request_file{ .source = source1.string(),
                              .destination = dest_dir / "dest1.txt" },
    envy::fetch_request_file{ .source = source2.string(),
                              .destination = dest_dir / "dest2.txt" },
    envy::fetch_request_file{ .source = source3.string(),
                              .destination = dest_dir / "dest3.txt" }
  };

  auto const results = envy::fetch(requests);

  REQUIRE(results.size() == 3);

  for (size_t i = 0; i < results.size(); ++i) {
    INFO("Checking result ", i);
    REQUIRE(std::holds_alternative<envy::fetch_result>(results[i]));
    auto const &result = std::get<envy::fetch_result>(results[i]);
    CHECK(std::filesystem::exists(result.resolved_destination));
  }

  CHECK(std::filesystem::exists(dest_dir / "dest1.txt"));
  CHECK(std::filesystem::exists(dest_dir / "dest2.txt"));
  CHECK(std::filesystem::exists(dest_dir / "dest3.txt"));

  // Cleanup
  std::filesystem::remove_all(temp_dir);
}

// --- fetch_request_from_url tests ---

TEST_CASE("fetch_request_from_url HTTP") {
  auto req{ envy::fetch_request_from_url("http://example.com/file.tar.gz",
                                         "/tmp/file.tar.gz") };
  REQUIRE(std::holds_alternative<envy::fetch_request_http>(req));
  auto const &r{ std::get<envy::fetch_request_http>(req) };
  CHECK(r.source == "http://example.com/file.tar.gz");
  CHECK(r.destination == "/tmp/file.tar.gz");
  CHECK_FALSE(r.post_data.has_value());
}

TEST_CASE("fetch_request_from_url HTTPS") {
  auto req{ envy::fetch_request_from_url("https://example.com/file.tar.gz",
                                         "/tmp/file.tar.gz") };
  REQUIRE(std::holds_alternative<envy::fetch_request_https>(req));
  auto const &r{ std::get<envy::fetch_request_https>(req) };
  CHECK(r.source == "https://example.com/file.tar.gz");
  CHECK(r.destination == "/tmp/file.tar.gz");
  CHECK_FALSE(r.post_data.has_value());
}

TEST_CASE("fetch_request_from_url FTP") {
  auto req{ envy::fetch_request_from_url("ftp://files.example.com/pub/archive.tar.gz",
                                         "/tmp/archive.tar.gz") };
  REQUIRE(std::holds_alternative<envy::fetch_request_ftp>(req));
  auto const &r{ std::get<envy::fetch_request_ftp>(req) };
  CHECK(r.source == "ftp://files.example.com/pub/archive.tar.gz");
  CHECK(r.destination == "/tmp/archive.tar.gz");
}

TEST_CASE("fetch_request_from_url FTPS") {
  auto req{ envy::fetch_request_from_url("ftps://secure.example.com/file.bin",
                                         "/tmp/file.bin") };
  REQUIRE(std::holds_alternative<envy::fetch_request_ftps>(req));
  auto const &r{ std::get<envy::fetch_request_ftps>(req) };
  CHECK(r.source == "ftps://secure.example.com/file.bin");
  CHECK(r.destination == "/tmp/file.bin");
}

TEST_CASE("fetch_request_from_url S3") {
  auto req{ envy::fetch_request_from_url("s3://my-bucket/path/to/object.tar.zst",
                                         "/tmp/object.tar.zst") };
  REQUIRE(std::holds_alternative<envy::fetch_request_s3>(req));
  auto const &r{ std::get<envy::fetch_request_s3>(req) };
  CHECK(r.source == "s3://my-bucket/path/to/object.tar.zst");
  CHECK(r.destination == "/tmp/object.tar.zst");
  CHECK(r.region.empty());
}

TEST_CASE("fetch_request_from_url local file absolute") {
  auto req{ envy::fetch_request_from_url("/usr/local/share/file.tar.gz",
                                         "/tmp/file.tar.gz") };
  REQUIRE(std::holds_alternative<envy::fetch_request_file>(req));
  auto const &r{ std::get<envy::fetch_request_file>(req) };
  CHECK(r.source == "/usr/local/share/file.tar.gz");
  CHECK(r.destination == "/tmp/file.tar.gz");
  CHECK(r.file_root.empty());
}

TEST_CASE("fetch_request_from_url local file relative") {
  auto req{ envy::fetch_request_from_url("relative/path/file.tar.gz",
                                         "/tmp/file.tar.gz") };
  REQUIRE(std::holds_alternative<envy::fetch_request_file>(req));
  auto const &r{ std::get<envy::fetch_request_file>(req) };
  CHECK(r.source == "relative/path/file.tar.gz");
  CHECK(r.destination == "/tmp/file.tar.gz");
  CHECK(r.file_root.empty());
}

TEST_CASE("fetch_request_from_url file:// scheme absolute") {
  auto req{ envy::fetch_request_from_url("file:///usr/local/file.tar.gz",
                                         "/tmp/file.tar.gz") };
  REQUIRE(std::holds_alternative<envy::fetch_request_file>(req));
  auto const &r{ std::get<envy::fetch_request_file>(req) };
  CHECK(r.destination == "/tmp/file.tar.gz");
}

TEST_CASE("fetch_request_from_url git:// throws") {
  CHECK_THROWS_AS(
      envy::fetch_request_from_url("git://github.com/user/repo.git", "/tmp/repo"),
      std::runtime_error);
}

TEST_CASE("fetch_request_from_url git+ssh:// throws") {
  CHECK_THROWS_AS(
      envy::fetch_request_from_url("git+ssh://github.com/user/repo.git", "/tmp/repo"),
      std::runtime_error);
}

TEST_CASE("fetch_request_from_url HTTPS .git suffix throws") {
  CHECK_THROWS_AS(
      envy::fetch_request_from_url("https://github.com/user/repo.git", "/tmp/repo"),
      std::runtime_error);
}

TEST_CASE("fetch_request_from_url SSH throws") {
  CHECK_THROWS_AS(envy::fetch_request_from_url("ssh://host/path", "/tmp/file"),
                  std::runtime_error);
}

TEST_CASE("fetch_request_from_url SCP-style SSH throws") {
  CHECK_THROWS_AS(envy::fetch_request_from_url("user@host:path/to/repo.git", "/tmp/repo"),
                  std::runtime_error);
}

TEST_CASE("fetch_request_from_url unknown scheme throws") {
  CHECK_THROWS_AS(
      envy::fetch_request_from_url("frobnicate://example.com/file", "/tmp/file"),
      std::runtime_error);
}

TEST_CASE("fetch_request_from_url error message includes URL") {
  try {
    envy::fetch_request_from_url("git://github.com/user/repo.git", "/tmp/repo");
    FAIL("Expected exception");
  } catch (std::runtime_error const &e) {
    CHECK(std::string(e.what()).find("git://github.com/user/repo.git") !=
          std::string::npos);
  }
}

TEST_CASE("fetch_request_from_url preserves source and dest exactly") {
  std::string const url{ "https://cdn.example.com/path/with%20spaces/file.tar.zst" };
  std::filesystem::path const dest{ "/some/deep/nested/output/dir/file.tar.zst" };
  auto req{ envy::fetch_request_from_url(url, dest) };
  REQUIRE(std::holds_alternative<envy::fetch_request_https>(req));
  auto const &r{ std::get<envy::fetch_request_https>(req) };
  CHECK(r.source == url);
  CHECK(r.destination == dest);
}

TEST_CASE("fetch_request_from_url case insensitive scheme") {
  SUBCASE("HTTP uppercase") {
    auto req{ envy::fetch_request_from_url("HTTP://EXAMPLE.COM/FILE", "/tmp/f") };
    CHECK(std::holds_alternative<envy::fetch_request_http>(req));
  }
  SUBCASE("Https mixed case") {
    auto req{ envy::fetch_request_from_url("Https://Example.Com/File", "/tmp/f") };
    CHECK(std::holds_alternative<envy::fetch_request_https>(req));
  }
  SUBCASE("FTP uppercase") {
    auto req{ envy::fetch_request_from_url("FTP://example.com/file", "/tmp/f") };
    CHECK(std::holds_alternative<envy::fetch_request_ftp>(req));
  }
  SUBCASE("S3 uppercase") {
    auto req{ envy::fetch_request_from_url("S3://bucket/key", "/tmp/f") };
    CHECK(std::holds_alternative<envy::fetch_request_s3>(req));
  }
}

TEST_CASE("fetch_request_from_url HTTP does not set post_data or progress") {
  auto req{ envy::fetch_request_from_url("http://example.com/f", "/tmp/f") };
  auto const &r{ std::get<envy::fetch_request_http>(req) };
  CHECK_FALSE(r.post_data.has_value());
  CHECK_FALSE(r.progress);
}

TEST_CASE("fetch_request_from_url S3 does not set region") {
  auto req{ envy::fetch_request_from_url("s3://bucket/key", "/tmp/f") };
  auto const &r{ std::get<envy::fetch_request_s3>(req) };
  CHECK(r.region.empty());
}

TEST_CASE("fetch_request_from_url file does not set file_root") {
  auto req{ envy::fetch_request_from_url("/absolute/file", "/tmp/f") };
  auto const &r{ std::get<envy::fetch_request_file>(req) };
  CHECK(r.file_root.empty());
}
