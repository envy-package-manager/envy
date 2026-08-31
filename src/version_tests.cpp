#include "version.h"

#include "doctest.h"

#include <algorithm>
#include <string_view>

#ifndef ENVY_VERSION_STR
#error "ENVY_VERSION_STR must be defined by the build system"
#endif

namespace {

using envy::version_is_newer;

// Basic numeric ordering
TEST_CASE("version_is_newer: patch bump") { CHECK(version_is_newer("1.0.1", "1.0.0")); }

TEST_CASE("version_is_newer: minor bump") { CHECK(version_is_newer("1.1.0", "1.0.0")); }

TEST_CASE("version_is_newer: major bump") { CHECK(version_is_newer("2.0.0", "1.0.0")); }

// Equal versions
TEST_CASE("version_is_newer: equal versions") {
  CHECK_FALSE(version_is_newer("1.2.3", "1.2.3"));
}

// Pre-release ordering
TEST_CASE("version_is_newer: alpha < beta") {
  CHECK(version_is_newer("1.0.0-beta", "1.0.0-alpha"));
}

TEST_CASE("version_is_newer: release beats pre-release") {
  CHECK(version_is_newer("1.0.0", "1.0.0-alpha"));
}

TEST_CASE("version_is_newer: pre-release < release") {
  CHECK_FALSE(version_is_newer("2.0.0-rc1", "2.0.0"));
}

// Major/minor/patch boundaries
TEST_CASE("version_is_newer: 1.9.9 < 2.0.0") { CHECK(version_is_newer("2.0.0", "1.9.9")); }

TEST_CASE("version_is_newer: 0.99.99 < 1.0.0") {
  CHECK(version_is_newer("1.0.0", "0.99.99"));
}

// Dev build (0.0.0)
TEST_CASE("version_is_newer: anything beats 0.0.0") {
  CHECK(version_is_newer("0.0.1", "0.0.0"));
}

TEST_CASE("version_is_newer: 0.0.0 vs 0.0.0") {
  CHECK_FALSE(version_is_newer("0.0.0", "0.0.0"));
}

// Candidate parse failure
TEST_CASE("version_is_newer: unparseable candidate") {
  CHECK_FALSE(version_is_newer("garbage", "1.0.0"));
}

TEST_CASE("version_is_newer: empty candidate") {
  CHECK_FALSE(version_is_newer("", "1.0.0"));
}

// Current parse failure
TEST_CASE("version_is_newer: unparseable current") {
  CHECK(version_is_newer("1.0.0", "garbage"));
}

TEST_CASE("version_is_newer: empty current") { CHECK(version_is_newer("1.0.0", "")); }

// Both unparseable
TEST_CASE("version_is_newer: both unparseable") {
  CHECK_FALSE(version_is_newer("garbage", "also-garbage"));
}

TEST_CASE("version_is_newer: both empty") { CHECK_FALSE(version_is_newer("", "")); }

// Downgrade prevention
TEST_CASE("version_is_newer: downgrade blocked") {
  CHECK_FALSE(version_is_newer("1.0.0", "2.0.0"));
}

// Whitespace trimming (file may have trailing newline)
TEST_CASE("version_is_newer: trailing newline in current") {
  CHECK(version_is_newer("2.0.0", "1.0.0\n"));
}

TEST_CASE("version_is_newer: trailing newline in candidate") {
  CHECK(version_is_newer("2.0.0\n", "1.0.0"));
}

TEST_CASE("version_is_newer: leading/trailing whitespace") {
  CHECK(version_is_newer("  2.0.0  ", "  1.0.0  "));
}

TEST_CASE("version_is_newer: CRLF in current") {
  CHECK(version_is_newer("2.0.0", "1.0.0\r\n"));
}

// The stamp itself, not the comparator. Nothing in-process can tell 0.0.0 from a release
// -- that is the tag check in functional_tests/test_version_stamp.py -- but a stamp that
// never made it through the build system is a shape, and shapes are checkable here.
TEST_CASE("ENVY_VERSION_STR: three dotted numeric components") {
  std::string_view const v{ ENVY_VERSION_STR };
  REQUIRE_FALSE(v.empty());
  CHECK(v.find("@@") == std::string_view::npos);  // unsubstituted template placeholder
  CHECK(std::ranges::count(v, '.') == 2);
  CHECK(std::ranges::all_of(v, [](char c) { return (c >= '0' && c <= '9') || c == '.'; }));
}

}  // namespace
