#include "package_depot.h"

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

using namespace envy;

namespace {
// Reusable SHA256 hex strings for test manifests
constexpr char kHash1[] =
    "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2";
constexpr char kHash2[] =
    "1111111111111111111111111111111111111111111111111111111111111111";
constexpr char kHash3[] =
    "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
constexpr char kHashA[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr char kHashB[] =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
}  // namespace

TEST_CASE("package_depot_index: empty index") {
  auto index{ package_depot_index::build_from_contents({}) };
  CHECK(index.empty());
  CHECK_FALSE(index.find("pkg@v1", "darwin", "arm64", "abcdef01").has_value());
}

TEST_CASE("package_depot_index: single manifest with one entry") {
  auto index{ package_depot_index::build_from_contents(
      { "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2  "
        "https://cdn.example.com/"
        "arm.gcc@r2-darwin-arm64-blake3-abcdef0123456789.tar.zst\n" }) };

  CHECK_FALSE(index.empty());

  auto result{ index.find("arm.gcc@r2", "darwin", "arm64", "abcdef0123456789") };
  REQUIRE(result.has_value());
  CHECK(result->url ==
        "https://cdn.example.com/arm.gcc@r2-darwin-arm64-blake3-abcdef0123456789.tar.zst");
  REQUIRE(result->sha256.has_value());
  CHECK(*result->sha256 == kHash1);
}

TEST_CASE("package_depot_index: miss on wrong identity") {
  auto index{ package_depot_index::build_from_contents(
      { "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2  "
        "https://cdn.example.com/"
        "arm.gcc@r2-darwin-arm64-blake3-abcdef0123456789.tar.zst\n" }) };

  CHECK_FALSE(index.find("arm.gcc@r3", "darwin", "arm64", "abcdef0123456789").has_value());
}

TEST_CASE("package_depot_index: miss on wrong platform") {
  auto index{ package_depot_index::build_from_contents(
      { "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2  "
        "https://cdn.example.com/"
        "arm.gcc@r2-darwin-arm64-blake3-abcdef0123456789.tar.zst\n" }) };

  CHECK_FALSE(index.find("arm.gcc@r2", "linux", "arm64", "abcdef0123456789").has_value());
}

TEST_CASE("package_depot_index: miss on wrong hash") {
  auto index{ package_depot_index::build_from_contents(
      { "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2  "
        "https://cdn.example.com/"
        "arm.gcc@r2-darwin-arm64-blake3-abcdef0123456789.tar.zst\n" }) };

  CHECK_FALSE(index.find("arm.gcc@r2", "darwin", "arm64", "0000000000000000").has_value());
}

TEST_CASE("package_depot_index: multiple entries in one manifest") {
  auto index{ package_depot_index::build_from_contents(
      { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
        "s3://bucket/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  "
        "s3://bucket/local.uv@r0-linux-x86_64-blake3-bbbb.tar.zst\n" }) };

  auto r1{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(r1.has_value());
  CHECK(r1->url == "s3://bucket/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst");

  auto r2{ index.find("local.uv@r0", "linux", "x86_64", "bbbb") };
  REQUIRE(r2.has_value());
  CHECK(r2->url == "s3://bucket/local.uv@r0-linux-x86_64-blake3-bbbb.tar.zst");
}

TEST_CASE("package_depot_index: blank lines and comments ignored") {
  auto index{ package_depot_index::build_from_contents(
      { "# This is a comment\n"
        "\n"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
        "s3://bucket/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n"
        "\n"
        "# Another comment\n"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  "
        "s3://bucket/local.uv@r0-linux-x86_64-blake3-bbbb.tar.zst\n" }) };

  CHECK(index.find("arm.gcc@r2", "darwin", "arm64", "aaaa").has_value());
  CHECK(index.find("local.uv@r0", "linux", "x86_64", "bbbb").has_value());
}

TEST_CASE("package_depot_index: invalid lines skipped") {
  auto index{ package_depot_index::build_from_contents(
      { "garbage-nonsense\n"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
        "s3://bucket/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n"
        "not-a-valid-archive.txt\n" }) };

  CHECK(index.find("arm.gcc@r2", "darwin", "arm64", "aaaa").has_value());
}

TEST_CASE("package_depot_index: multiple manifests searched in order") {
  auto index{ package_depot_index::build_from_contents(
      { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
        "https://depot1/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n",
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  "
        "https://depot2/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n" }) };

  // First manifest wins
  auto result{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(result.has_value());
  CHECK(result->url == "https://depot1/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst");
}

TEST_CASE("package_depot_index: disjoint manifests both consulted") {
  auto index{ package_depot_index::build_from_contents(
      { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
        "https://depot1/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n",
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  "
        "https://depot2/local.uv@r0-linux-x86_64-blake3-bbbb.tar.zst\n" }) };

  CHECK(index.find("arm.gcc@r2", "darwin", "arm64", "aaaa").has_value());
  CHECK(index.find("local.uv@r0", "linux", "x86_64", "bbbb").has_value());
}

TEST_CASE("package_depot_index: first manifest with match stops search") {
  auto index{ package_depot_index::build_from_contents(
      { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
        "https://depot1/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n",
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  "
        "https://depot2/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n"
        "1111111111111111111111111111111111111111111111111111111111111111  "
        "https://depot2/local.uv@r0-linux-x86_64-blake3-bbbb.tar.zst\n" }) };

  // pkg A found in first manifest
  auto r1{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(r1.has_value());
  CHECK(r1->url == "https://depot1/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst");

  // pkg B only in second manifest — still found
  auto r2{ index.find("local.uv@r0", "linux", "x86_64", "bbbb") };
  REQUIRE(r2.has_value());
  CHECK(r2->url == "https://depot2/local.uv@r0-linux-x86_64-blake3-bbbb.tar.zst");
}

TEST_CASE("package_depot_index: empty manifest text yields empty index") {
  auto index{ package_depot_index::build_from_contents({ "", "\n\n" }) };
  CHECK(index.empty());
}

TEST_CASE("package_depot_index: S3 URLs parsed correctly") {
  auto index{ package_depot_index::build_from_contents(
      { "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789  "
        "s3://my-bucket/cache/"
        "arm.gcc@r2-darwin-arm64-blake3-abcdef0123456789.tar.zst\n" }) };

  auto result{ index.find("arm.gcc@r2", "darwin", "arm64", "abcdef0123456789") };
  REQUIRE(result.has_value());
  CHECK(result->url ==
        "s3://my-bucket/cache/arm.gcc@r2-darwin-arm64-blake3-abcdef0123456789.tar.zst");
}

TEST_CASE("package_depot_index: Windows line endings handled") {
  auto index{ package_depot_index::build_from_contents(
      { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
        "https://cdn/pkg@v1-darwin-arm64-blake3-aaaa.tar.zst\r\n"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  "
        "https://cdn/pkg@v2-linux-x86_64-blake3-bbbb.tar.zst\r\n" }) };

  CHECK(index.find("pkg@v1", "darwin", "arm64", "aaaa").has_value());
  CHECK(index.find("pkg@v2", "linux", "x86_64", "bbbb").has_value());
}

TEST_CASE("package_depot_index: miss on wrong arch") {
  auto index{ package_depot_index::build_from_contents(
      { "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2  "
        "https://cdn.example.com/"
        "arm.gcc@r2-darwin-arm64-blake3-abcdef0123456789.tar.zst\n" }) };

  CHECK_FALSE(
      index.find("arm.gcc@r2", "darwin", "x86_64", "abcdef0123456789").has_value());
}

TEST_CASE("package_depot_index: duplicate entries in same manifest keeps first") {
  auto index{ package_depot_index::build_from_contents(
      { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
        "https://first/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  "
        "https://second/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n" }) };

  auto result{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(result.has_value());
  CHECK(result->url == "https://first/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst");
  REQUIRE(result->sha256.has_value());
  CHECK(*result->sha256 == kHashA);
}

TEST_CASE("package_depot_index: whitespace-only lines skipped") {
  auto index{ package_depot_index::build_from_contents(
      { "   \n"
        "\t\n"
        "  \t  \n"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
        "https://cdn/pkg@v1-darwin-arm64-blake3-aaaa.tar.zst\n" }) };

  CHECK(index.find("pkg@v1", "darwin", "arm64", "aaaa").has_value());
}

TEST_CASE("package_depot_index: bare filename with SHA256") {
  auto index{ package_depot_index::build_from_contents(
      { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
        "arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n" }) };

  auto result{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(result.has_value());
  CHECK(result->url == "arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst");
}

TEST_CASE("package_depot_index: a labelled archive is found by its bare identity") {
  auto index{ package_depot_index::build_from_contents(
      { std::string{ kHashA } +
        "  https://cdn.example.com/"
        "fi.github+libusb-hidapi@r1-darwin-arm64-blake3-aaaa.tar.zst\n" }) };

  auto const result{ index.find("fi.github@r1", "darwin", "arm64", "aaaa") };
  REQUIRE(result.has_value());
  CHECK(result->url ==
        "https://cdn.example.com/"
        "fi.github+libusb-hidapi@r1-darwin-arm64-blake3-aaaa.tar.zst");
}

TEST_CASE("package_depot_index: manifest with only comments yields empty index") {
  auto index{ package_depot_index::build_from_contents(
      { "# This is just comments\n"
        "# Nothing real here\n" }) };

  CHECK(index.empty());
}

TEST_CASE("package_depot_index: lines without .tar.zst extension skipped") {
  auto index{ package_depot_index::build_from_contents(
      { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
        "https://cdn/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.gz\n"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  "
        "https://cdn/pkg@v1-darwin-arm64-blake3-bbbb.tar.zst\n" }) };

  CHECK_FALSE(index.find("arm.gcc@r2", "darwin", "arm64", "aaaa").has_value());
  CHECK(index.find("pkg@v1", "darwin", "arm64", "bbbb").has_value());
}

TEST_CASE("package_depot_index: find with empty identity returns nullopt") {
  auto index{ package_depot_index::build_from_contents(
      { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
        "https://cdn/pkg@v1-darwin-arm64-blake3-aaaa.tar.zst\n" }) };

  CHECK_FALSE(index.find("", "darwin", "arm64", "aaaa").has_value());
}

TEST_CASE("package_depot_index: find with empty hash returns nullopt") {
  auto index{ package_depot_index::build_from_contents(
      { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
        "https://cdn/pkg@v1-darwin-arm64-blake3-aaaa.tar.zst\n" }) };

  CHECK_FALSE(index.find("pkg@v1", "darwin", "arm64", "").has_value());
}

TEST_CASE("package_depot_index: URL with deep path structure") {
  auto index{ package_depot_index::build_from_contents(
      { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
        "s3://bucket/a/b/c/d/e/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n" }) };

  auto result{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(result.has_value());
  CHECK(result->url ==
        "s3://bucket/a/b/c/d/e/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst");
}

TEST_CASE("package_depot_index: build with unsupported URL returns empty index") {
  namespace fs = std::filesystem;
  auto const tmp{ fs::temp_directory_path() / "envy-depot-test" };
  std::error_code ec;
  fs::create_directories(tmp, ec);
  auto index{ package_depot_index::build({ "gopher://invalid/depot.txt" }, tmp) };
  fs::remove_all(tmp, ec);
  CHECK(index.empty());
}

// --- Plain URL rejection tests ---

TEST_CASE("package_depot_index: plain URL line is rejected") {
  auto index{ package_depot_index::build_from_contents(
      { "https://cdn/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n" }) };

  // Plain URL without SHA256 is now rejected
  CHECK(index.empty());
}

TEST_CASE("package_depot_index: mixed SHA256 and plain lines — only SHA256 kept") {
  auto index{ package_depot_index::build_from_contents(
      { "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2  "
        "https://cdn/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n"
        "https://cdn/local.uv@r0-linux-x86_64-blake3-bbbb.tar.zst\n" }) };

  auto r1{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(r1.has_value());
  REQUIRE(r1->sha256.has_value());

  // Plain line rejected
  CHECK_FALSE(index.find("local.uv@r0", "linux", "x86_64", "bbbb").has_value());
}

TEST_CASE(
    "package_depot_index: 63-char hex prefix not treated as SHA256 — line rejected") {
  auto index{ package_depot_index::build_from_contents(
      { "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b  "
        "https://cdn/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n" }) };

  // 63 hex chars: no SHA256, line treated as plain URL → rejected
  CHECK(index.empty());
}

TEST_CASE("package_depot_index: 64 hex chars with single space — line rejected") {
  auto index{ package_depot_index::build_from_contents(
      { "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2 "
        "https://cdn/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n" }) };

  // Single space — no SHA256 → rejected
  CHECK(index.empty());
}

TEST_CASE("package_depot_index: 64 non-hex chars with two spaces — line rejected") {
  auto index{ package_depot_index::build_from_contents(
      { "g1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2  "
        "https://cdn/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n" }) };

  // 'g' not hex — no SHA256 → rejected
  CHECK(index.empty());
}

// --- SHA256 manifest line tests ---

TEST_CASE("package_depot_index: SHA256 line parsed correctly") {
  auto index{ package_depot_index::build_from_contents(
      { "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2  "
        "https://cdn/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n" }) };

  auto result{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(result.has_value());
  CHECK(result->url == "https://cdn/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst");
  REQUIRE(result->sha256.has_value());
  CHECK(*result->sha256 == kHash1);
}

TEST_CASE("package_depot_index: SHA256 uppercase hex normalized to lowercase") {
  auto index{ package_depot_index::build_from_contents(
      { "A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5F6A1B2  "
        "https://cdn/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n" }) };

  auto result{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(result.has_value());
  REQUIRE(result->sha256.has_value());
  CHECK(*result->sha256 == kHash1);
}

TEST_CASE("package_depot_index: SHA256 mixed-case hex normalized") {
  auto index{ package_depot_index::build_from_contents(
      { "A1b2C3d4E5f6A1b2C3d4E5f6A1b2C3d4E5f6A1b2C3d4E5f6A1b2C3d4E5f6A1b2  "
        "https://cdn/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n" }) };

  auto result{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(result.has_value());
  REQUIRE(result->sha256.has_value());
  CHECK(*result->sha256 == kHash1);
}

TEST_CASE("package_depot_index: SHA256 with S3 URL") {
  auto index{ package_depot_index::build_from_contents(
      { "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789  "
        "s3://my-bucket/cache/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n" }) };

  auto result{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(result.has_value());
  CHECK(result->url == "s3://my-bucket/cache/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst");
  REQUIRE(result->sha256.has_value());
  CHECK(*result->sha256 == kHash3);
}

TEST_CASE("package_depot_index: SHA256 with Windows line endings") {
  auto index{ package_depot_index::build_from_contents(
      { "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789  "
        "https://cdn/pkg@v1-darwin-arm64-blake3-aaaa.tar.zst\r\n" }) };

  auto result{ index.find("pkg@v1", "darwin", "arm64", "aaaa") };
  REQUIRE(result.has_value());
  REQUIRE(result->sha256.has_value());
  CHECK(*result->sha256 == kHash3);
}

TEST_CASE("package_depot_index: SHA256 manifest with comments and blank lines") {
  auto index{ package_depot_index::build_from_contents(
      { "# Depot manifest with SHA256\n"
        "\n"
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789  "
        "https://cdn/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n"
        "\n"
        "# Another entry\n"
        "1111111111111111111111111111111111111111111111111111111111111111  "
        "https://cdn/local.uv@r0-linux-x86_64-blake3-bbbb.tar.zst\n" }) };

  auto r1{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(r1.has_value());
  REQUIRE(r1->sha256.has_value());

  auto r2{ index.find("local.uv@r0", "linux", "x86_64", "bbbb") };
  REQUIRE(r2.has_value());
  REQUIRE(r2->sha256.has_value());
  CHECK(*r2->sha256 == kHash2);
}

TEST_CASE("package_depot_index: SHA256 duplicate entries keeps first") {
  auto index{ package_depot_index::build_from_contents(
      { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
        "https://first/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  "
        "https://second/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n" }) };

  auto result{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(result.has_value());
  CHECK(result->url == "https://first/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst");
  REQUIRE(result->sha256.has_value());
  CHECK(*result->sha256 == kHashA);
}

TEST_CASE("package_depot_index: SHA256 in both manifests") {
  auto index{ package_depot_index::build_from_contents(
      { "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789  "
        "https://depot1/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n",
        "1111111111111111111111111111111111111111111111111111111111111111  "
        "https://depot2/local.uv@r0-linux-x86_64-blake3-bbbb.tar.zst\n" }) };

  auto r1{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(r1.has_value());
  REQUIRE(r1->sha256.has_value());

  auto r2{ index.find("local.uv@r0", "linux", "x86_64", "bbbb") };
  REQUIRE(r2.has_value());
  REQUIRE(r2->sha256.has_value());
}

// --- build_from_directory tests (no SHA256 required) ---

TEST_CASE("package_depot_index: build_from_directory with checksums") {
  namespace fs = std::filesystem;
  auto const tmp{ fs::temp_directory_path() / "envy-depot-dir-cksum-test" };
  std::error_code ec;
  fs::create_directories(tmp, ec);

  // Create a fake .tar.zst file
  std::string const filename{ "arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst" };
  {
    std::ofstream f{ tmp / filename };
    f << "fake";
  }

  std::unordered_map<std::string, std::string> checksums;
  checksums[filename] = kHashA;

  auto index{ package_depot_index::build_from_directory(tmp, checksums) };
  fs::remove_all(tmp, ec);

  CHECK_FALSE(index.empty());
  auto result{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(result.has_value());
  REQUIRE(result->sha256.has_value());
  CHECK(*result->sha256 == kHashA);
}

TEST_CASE("package_depot_index: build_from_directory without checksums has no sha256") {
  namespace fs = std::filesystem;
  auto const tmp{ fs::temp_directory_path() / "envy-depot-dir-nocksum-test" };
  std::error_code ec;
  fs::create_directories(tmp, ec);

  std::string const filename{ "arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst" };
  {
    std::ofstream f{ tmp / filename };
    f << "fake";
  }

  auto index{ package_depot_index::build_from_directory(tmp) };
  fs::remove_all(tmp, ec);

  CHECK_FALSE(index.empty());
  auto result{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(result.has_value());
  CHECK_FALSE(result->sha256.has_value());
}

// --- merge tests ---

TEST_CASE("package_depot_index: merge combines disjoint indexes") {
  auto index{ package_depot_index::build_from_text(
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
      "https://depot1/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n",
      true) };
  index.merge(package_depot_index::build_from_text(
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  "
      "https://depot2/local.uv@r0-linux-x86_64-blake3-bbbb.tar.zst\n",
      true));

  CHECK(index.find("arm.gcc@r2", "darwin", "arm64", "aaaa").has_value());
  CHECK(index.find("local.uv@r0", "linux", "x86_64", "bbbb").has_value());
}

TEST_CASE("package_depot_index: merge duplicate key identical sha keeps entry") {
  auto index{ package_depot_index::build_from_text(
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
      "https://first/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n",
      true) };
  index.merge(package_depot_index::build_from_text(
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
      "https://second/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n",
      true));

  auto result{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(result.has_value());
  CHECK(result->url == "https://first/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst");
  CHECK(*result->sha256 == kHashA);
}

TEST_CASE("package_depot_index: merge duplicate key differing sha keeps first") {
  auto index{ package_depot_index::build_from_text(
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
      "https://first/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n",
      true) };
  index.merge(package_depot_index::build_from_text(
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  "
      "https://second/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n",
      true));

  auto result{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(result.has_value());
  CHECK(result->url == "https://first/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst");
  CHECK(*result->sha256 == kHashA);
}

TEST_CASE("package_depot_index: merge duplicate key missing sha keeps first") {
  auto index{ package_depot_index::build_from_text(
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
      "https://first/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n",
      true) };
  index.merge(package_depot_index::build_from_text(
      "https://second/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n",
      false));

  auto result{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(result.has_value());
  CHECK(result->url == "https://first/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst");
  REQUIRE(result->sha256.has_value());
}

// --- build_from_text tests ---

TEST_CASE(
    "package_depot_index: build_from_text without sha requirement accepts plain "
    "URLs") {
  auto index{ package_depot_index::build_from_text(
      "/local/path/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst\n",
      false) };

  auto result{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(result.has_value());
  CHECK(result->url == "/local/path/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst");
  CHECK_FALSE(result->sha256.has_value());
}

TEST_CASE("package_depot_index: build_from_text parses identically to contents form") {
  std::string const text{
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
    "https://cdn/pkg@v1-darwin-arm64-blake3-aaaa.tar.zst\n"
  };
  auto from_text{ package_depot_index::build_from_text(text, true) };
  auto from_contents{ package_depot_index::build_from_contents({ text }) };

  auto r1{ from_text.find("pkg@v1", "darwin", "arm64", "aaaa") };
  auto r2{ from_contents.find("pkg@v1", "darwin", "arm64", "aaaa") };
  REQUIRE(r1.has_value());
  REQUIRE(r2.has_value());
  CHECK(r1->url == r2->url);
  CHECK(r1->sha256 == r2->sha256);
}

// --- build_from_entries tests ---

TEST_CASE("package_depot_index: build_from_entries keys by filename stem") {
  auto index{ package_depot_index::build_from_entries(
      { { "https://cdn/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst",
          std::string{ kHashA } },
        { "/local/local.uv@r0-linux-x86_64-blake3-bbbb.tar.zst", std::nullopt } }) };

  auto r1{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(r1.has_value());
  CHECK(r1->url == "https://cdn/arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst");
  REQUIRE(r1->sha256.has_value());
  CHECK(*r1->sha256 == kHashA);

  auto r2{ index.find("local.uv@r0", "linux", "x86_64", "bbbb") };
  REQUIRE(r2.has_value());
  CHECK_FALSE(r2->sha256.has_value());
}

TEST_CASE("package_depot_index: build_from_entries normalizes sha to lowercase") {
  auto index{ package_depot_index::build_from_entries(
      { { "https://cdn/pkg@v1-darwin-arm64-blake3-aaaa.tar.zst",
          std::string{ "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                       "A" } } }) };

  auto result{ index.find("pkg@v1", "darwin", "arm64", "aaaa") };
  REQUIRE(result.has_value());
  CHECK(*result->sha256 == kHashA);
}

TEST_CASE("package_depot_index: build_from_entries rejects empty url") {
  CHECK_THROWS_WITH_AS(package_depot_index::build_from_entries({ { "", std::nullopt } }),
                       doctest::Contains("non-empty 'url'"),
                       std::runtime_error);
}

TEST_CASE("package_depot_index: build_from_entries rejects malformed sha256") {
  CHECK_THROWS_WITH_AS(package_depot_index::build_from_entries(
                           { { "https://cdn/pkg@v1-darwin-arm64-blake3-aaaa.tar.zst",
                               std::string{ "nothex" } } }),
                       doctest::Contains("malformed sha256"),
                       std::runtime_error);
}

TEST_CASE("package_depot_index: build_from_entries skips non-tar-zst urls") {
  auto index{ package_depot_index::build_from_entries(
      { { "https://cdn/pkg@v1-darwin-arm64-blake3-aaaa.tar.gz", std::nullopt },
        { "https://cdn/pkg@v2-darwin-arm64-blake3-bbbb.tar.zst", std::nullopt } }) };

  CHECK_FALSE(index.find("pkg@v1", "darwin", "arm64", "aaaa").has_value());
  CHECK(index.find("pkg@v2", "darwin", "arm64", "bbbb").has_value());
}

TEST_CASE("package_depot_index: build_from_entries skips unparseable stems") {
  auto index{ package_depot_index::build_from_entries(
      { { "https://cdn/not-a-cache-key.tar.zst", std::nullopt } }) };

  CHECK(index.empty());
}

TEST_CASE("package_depot_index: build_from_directory checksums partial match") {
  namespace fs = std::filesystem;
  auto const tmp{ fs::temp_directory_path() / "envy-depot-dir-partial-test" };
  std::error_code ec;
  fs::create_directories(tmp, ec);

  std::string const file_a{ "arm.gcc@r2-darwin-arm64-blake3-aaaa.tar.zst" };
  std::string const file_b{ "local.uv@r0-linux-x86_64-blake3-bbbb.tar.zst" };
  {
    std::ofstream fa{ tmp / file_a };
    fa << "fake-a";
    std::ofstream fb{ tmp / file_b };
    fb << "fake-b";
  }

  // Only provide checksum for file_a
  std::unordered_map<std::string, std::string> checksums;
  checksums[file_a] = kHashA;

  auto index{ package_depot_index::build_from_directory(tmp, checksums) };
  fs::remove_all(tmp, ec);

  auto r1{ index.find("arm.gcc@r2", "darwin", "arm64", "aaaa") };
  REQUIRE(r1.has_value());
  REQUIRE(r1->sha256.has_value());
  CHECK(*r1->sha256 == kHashA);

  auto r2{ index.find("local.uv@r0", "linux", "x86_64", "bbbb") };
  REQUIRE(r2.has_value());
  CHECK_FALSE(r2->sha256.has_value());
}
