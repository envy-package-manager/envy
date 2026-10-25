#include "semver.h"

#include "doctest.h"

#include <array>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace envy {
namespace {

TEST_CASE("semver_parse: success leaves why empty") {
  nsv_semver_t v{};
  std::string why{ "stale" };
  CHECK(semver_parse(v, "1.2.3-rc.1", &why) == NSV_RET_OK);
  CHECK(v.minor == 2);
  CHECK(why.empty());
}

TEST_CASE("semver_parse: failure says where and why") {
  nsv_semver_t v{};
  std::string why;
  CHECK(semver_parse(v, "1.02.0", &why) == NSV_RET_ERR_SYNTAX);
  CHECK(why == "\nbad version at column 3: leading zero\n  1.02.0\n    ^");
}

TEST_CASE("semver_parse: an empty view is a syntax error, explained") {
  nsv_semver_t v{};
  std::string why;
  CHECK(semver_parse(v, std::string_view{}, &why) == NSV_RET_ERR_SYNTAX);
  CHECK(why.starts_with("\nbad version at column 1: expected a number"));
}

TEST_CASE("semver_parse: without why a failure is dropped, not left for the next call") {
  nsv_semver_t v{};
  CHECK(semver_parse(v, "garbage") == NSV_RET_ERR_SYNTAX);
  std::string why;
  CHECK(semver_parse(v, "1.0.x", &why) == NSV_RET_ERR_SYNTAX);
  CHECK(why == "\nbad version at column 5: expected a number\n  1.0.x\n      ^");
}

TEST_CASE("semver_range_match: a malformed range says why; a well-formed one clears it") {
  nsv_semver_t v{};
  REQUIRE(semver_parse(v, "1.5.0") == NSV_RET_OK);
  std::string why;
  CHECK(semver_range_match("1.0.0 - 2.0.0", v, &why) == NSV_RET_ERR_SYNTAX);
  CHECK(why ==
        "\nbad range at column 7: hyphen ranges are not supported; use >= and <=\n"
        "  1.0.0 - 2.0.0\n"
        "        ^");
  CHECK(semver_range_match(">=1.0.0 <2.0.0", v, &why) == 1);
  CHECK(why.empty());
  CHECK(semver_range_match("<1.0.0", v, &why) == 0);
  CHECK(why.empty());
}

TEST_CASE("semver_range_match: a version past nanosemver's limits is explained") {
  nsv_semver_t const v{};
  std::string why;
  CHECK(semver_range_match(">=4294967296.0.0", v, &why) == NSV_RET_ERR_OVERFLOW);
  CHECK(why.starts_with("\nbad range at column 3: number over 4294967295"));
}

// The print hook is process-wide; each thread's message must land in its own why.
TEST_CASE("semver_parse: concurrent failures keep their own diagnostics") {
  constexpr int kThreads{ 8 };
  std::array<int, kThreads> mismatches{};
  std::vector<std::thread> threads;
  for (int t{ 0 }; t < kThreads; ++t) {
    threads.emplace_back([t, &mismatches] {
      std::string const input{ "1." + std::to_string(t) + ".x" };
      std::string const expect{ "\nbad version at column 5: expected a number\n  " +
                                input + "\n      ^" };
      for (int i{ 0 }; i < 500; ++i) {
        nsv_semver_t v{};
        std::string why;
        semver_parse(v, "noise");  // a dropped failure in between must not leak into why
        if (semver_parse(v, input, &why) != NSV_RET_ERR_SYNTAX || why != expect) {
          ++mismatches[t];
        }
      }
    });
  }
  for (auto &th : threads) { th.join(); }
  for (int t{ 0 }; t < kThreads; ++t) { CHECK(mismatches[t] == 0); }
}

}  // namespace
}  // namespace envy
