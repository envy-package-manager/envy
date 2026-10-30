#include "fetch_git.h"

#include "git2.h"

#include "doctest.h"

#include <string>
#include <vector>

namespace {

constexpr char kSha[]{ "0123456789abcdef0123456789abcdef01234567" };
constexpr char kOther[]{ "89abcdef0123456789abcdef0123456789abcdef" };

// A tip's plan, after checking it falls back to the same refspec at full depth.
envy::git_fetch_plan only_plan(std::vector<envy::git_ref_entry> const &refs) {
  auto const plans{ envy::git_fetch_plans(kSha, refs, false) };
  REQUIRE(plans.size() == 2);
  CHECK(plans[1].strategy == plans[0].strategy);
  CHECK(plans[1].refspecs == plans[0].refspecs);
  CHECK(plans[1].depth == 0);
  return plans.front();
}

}  // namespace

TEST_CASE("git_fetch_plans: a local source takes the commit at full depth") {
  auto const plans{ envy::git_fetch_plans(kSha, { { "refs/heads/main", kSha } }, true) };
  REQUIRE(plans.size() == 1);
  CHECK(plans[0].strategy == envy::git_fetch_strategy::LOCAL);
  CHECK(plans[0].refspecs == std::vector<std::string>{ kSha });
  CHECK(plans[0].depth == 0);
}

TEST_CASE("git_fetch_plans: an advertised tip is fetched by name, shallow first") {
  SUBCASE("lightweight tag keeps its name") {
    auto const plan{ only_plan({ { "refs/tags/v1", kSha } }) };
    CHECK(plan.strategy == envy::git_fetch_strategy::TIP);
    CHECK(plan.refspecs == std::vector<std::string>{ "+refs/tags/v1:refs/tags/v1" });
    CHECK(plan.depth == 1);
  }
  SUBCASE("annotated tag matches through its peeled entry") {
    auto const plan{ only_plan({ { "refs/tags/v1", kOther }, { "refs/tags/v1^{}", kSha } }) };
    CHECK(plan.refspecs == std::vector<std::string>{ "+refs/tags/v1:refs/tags/v1" });
  }
  SUBCASE("branch lands as a remote-tracking ref") {
    auto const plan{ only_plan({ { "refs/heads/main", kSha } }) };
    CHECK(plan.refspecs ==
          std::vector<std::string>{ "+refs/heads/main:refs/remotes/origin/main" });
  }
  SUBCASE("a tag beats a branch, a branch beats HEAD") {
    auto const plan{ only_plan(
        { { "HEAD", kSha }, { "refs/heads/main", kSha }, { "refs/tags/v1", kSha } }) };
    CHECK(plan.refspecs == std::vector<std::string>{ "+refs/tags/v1:refs/tags/v1" });
    CHECK(only_plan({ { "HEAD", kSha }, { "refs/heads/main", kSha } }).refspecs ==
          std::vector<std::string>{ "+refs/heads/main:refs/remotes/origin/main" });
  }
  SUBCASE("HEAD or an unconventional ref goes to FETCH_HEAD only") {
    CHECK(only_plan({ { "HEAD", kSha } }).refspecs == std::vector<std::string>{ "HEAD" });
    CHECK(only_plan({ { "refs/pull/7/head", kSha } }).refspecs ==
          std::vector<std::string>{ "refs/pull/7/head" });
  }
}

TEST_CASE("git_fetch_plans: an unadvertised commit tries OID, then FULL") {
  auto const plans{ envy::git_fetch_plans(kSha, { { "refs/heads/main", kOther } }, false) };
  REQUIRE(plans.size() == 2);
  CHECK(plans[0].strategy == envy::git_fetch_strategy::OID);
  CHECK(plans[0].refspecs == std::vector<std::string>{ kSha });
  CHECK(plans[0].depth == 1);
  CHECK(plans[1].strategy == envy::git_fetch_strategy::FULL);
  CHECK(plans[1].depth == 0);
  CHECK(plans[1].refspecs ==
        std::vector<std::string>{ "+refs/heads/*:refs/remotes/origin/*",
                                  "+refs/tags/*:refs/tags/*" });
}

TEST_CASE("git_fetch_strategy_name names every strategy") {
  CHECK(std::string{ envy::git_fetch_strategy_name(envy::git_fetch_strategy::LOCAL) } ==
        "local");
  CHECK(std::string{ envy::git_fetch_strategy_name(envy::git_fetch_strategy::TIP) } ==
        "tip");
  CHECK(std::string{ envy::git_fetch_strategy_name(envy::git_fetch_strategy::OID) } ==
        "oid");
  CHECK(std::string{ envy::git_fetch_strategy_name(envy::git_fetch_strategy::FULL) } ==
        "full");
}

TEST_CASE("git_fetch_target: a full sha or an advertised name, nothing else") {
  std::vector<envy::git_ref_entry> const refs{ { "refs/heads/main", kSha },
                                               { "refs/tags/v1", kOther } };

  CHECK(envy::git_fetch_target("0123456789ABCDEF0123456789ABCDEF01234567", refs) == kSha);
  CHECK(envy::git_fetch_target("main", refs) == kSha);
  CHECK(envy::git_fetch_target("refs/tags/v1", refs) == kOther);

  for (char const *bad : { "0123456789ab", "HEAD~1", "nope" }) {
    CAPTURE(bad);
    try {
      envy::git_fetch_target(bad, refs);
      FAIL("expected a throw");
    } catch (envy::fetch_error const &e) {
      CHECK(e.kind() == envy::fetch_error_kind::PROTOCOL);
      CHECK(std::string{ e.what() }.find("full commit sha") != std::string::npos);
    }
  }
}

TEST_CASE("git_url_is_local: paths and file:// only") {
  CHECK(envy::git_url_is_local("/srv/repos/a.git"));
  CHECK(envy::git_url_is_local("file:///srv/repos/a.git"));
  CHECK(envy::git_url_is_local("repos/a.git"));
  CHECK(envy::git_url_is_local("C:/repos/a.git"));
  CHECK(envy::git_url_is_local("C:\\repos\\a.git"));
  CHECK(envy::git_url_is_local("a/b:c.git"));

  CHECK_FALSE(envy::git_url_is_local("https://github.com/a/b.git"));
  CHECK_FALSE(envy::git_url_is_local("git://host/a.git"));
  CHECK_FALSE(envy::git_url_is_local("git+ssh://host/a.git"));
  CHECK_FALSE(envy::git_url_is_local("git@github.com:org/a.git"));
}

TEST_CASE("git_submodule_url resolves relative urls the way git does") {
  std::string const idf{ "https://github.com/espressif/esp-idf.git" };
  CHECK(envy::git_submodule_url(idf, "../../espressif/esp32-bt-lib.git") ==
        "https://github.com/espressif/esp32-bt-lib.git");
  CHECK(envy::git_submodule_url(idf + "/", "../../kmackay/micro-ecc.git") ==
        "https://github.com/kmackay/micro-ecc.git");
  CHECK(envy::git_submodule_url(idf, "../mbedtls.git") ==
        "https://github.com/espressif/mbedtls.git");
  CHECK(envy::git_submodule_url(idf, "./sub.git") ==
        "https://github.com/espressif/esp-idf.git/sub.git");
  CHECK(envy::git_submodule_url(idf, "./../x.git/") == "https://github.com/espressif/x.git");

  // Nested: a submodule's own url is the base for its children.
  CHECK(envy::git_submodule_url("git://127.0.0.1:9418/lib.git", "../leaf.git") ==
        "git://127.0.0.1:9418/leaf.git");

  // scp-style: past the last '/', the ':' separates host from path.
  CHECK(envy::git_submodule_url("git@github.com:org/repo.git", "../other.git") ==
        "git@github.com:org/other.git");
  CHECK(envy::git_submodule_url("git@github.com:org/repo.git", "../../x/y.git") ==
        "git@github.com:x/y.git");

  CHECK(envy::git_submodule_url("/srv/repos/super.git", "../lib.git") ==
        "/srv/repos/lib.git");
}

TEST_CASE("git_submodule_url passes absolute urls through") {
  for (char const *url : { "https://github.com/Mbed-TLS/mbedtls-framework",
                           "git@host:a/b.git",
                           "/abs/repo.git",
                           "file:///abs/repo.git" }) {
    CAPTURE(url);
    CHECK(envy::git_submodule_url("https://h/a/b.git", url) == url);
  }
}

TEST_CASE("git_submodule_url refuses to climb past the host") {
  CHECK_THROWS_WITH(envy::git_submodule_url("https://h/b.git", "../../c.git"),
                    doctest::Contains("climbs above"));
  CHECK_THROWS_WITH(envy::git_submodule_url("git@host:repo.git", "../../c.git"),
                    doctest::Contains("climbs above"));
}

TEST_CASE("git_error_kind: a server's refusal is final, a dropped link is not") {
  CHECK(envy::git_error_kind(GIT_ERROR_NET, "remote error: upload-pack: not our ref") ==
        envy::fetch_error_kind::PROTOCOL);
  CHECK(envy::git_error_kind(GIT_ERROR_INVALID,
                             "cannot fetch a specific object from the remote repository") ==
        envy::fetch_error_kind::PROTOCOL);
  CHECK(envy::git_error_kind(GIT_ERROR_REFERENCE, "x") == envy::fetch_error_kind::PROTOCOL);
  CHECK(envy::git_error_kind(GIT_ERROR_NET, "invalid packet line") ==
        envy::fetch_error_kind::PROTOCOL);
  CHECK(envy::git_error_kind(GIT_ERROR_HTTP,
                             "remote authentication required but no callback set") ==
        envy::fetch_error_kind::PROTOCOL);
  CHECK(envy::git_error_kind(GIT_ERROR_HTTP, "unexpected http status code: 404") ==
        envy::fetch_error_kind::PROTOCOL);
  CHECK(envy::git_error_kind(GIT_ERROR_HTTP, "unexpected http status code: 429") ==
        envy::fetch_error_kind::TRANSFER);
  CHECK(envy::git_error_kind(GIT_ERROR_HTTP, "unexpected http status code: 503") ==
        envy::fetch_error_kind::TRANSFER);
  CHECK(envy::git_error_kind(GIT_ERROR_NET, "connection reset") ==
        envy::fetch_error_kind::TRANSFER);
  CHECK(envy::git_error_kind(GIT_ERROR_HTTP, "x") == envy::fetch_error_kind::TRANSFER);
  CHECK(envy::git_error_kind(GIT_ERROR_INDEXER, "x") == envy::fetch_error_kind::TRANSFER);
  CHECK(envy::git_error_kind(GIT_ERROR_ODB, "x") == envy::fetch_error_kind::OTHER);
}
