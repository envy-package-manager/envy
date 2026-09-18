#include "vendor.h"

#include "doctest.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
fs::path const kRoot{ "C:/proj" };
#else
fs::path const kRoot{ "/proj" };
#endif

envy::vendor_request derived(char const *canonical) {
  return { .key = envy::pkg_key{ canonical }, .path_override = std::nullopt };
}

envy::vendor_request overridden(char const *canonical, char const *path) {
  return { .key = envy::pkg_key{ canonical }, .path_override = std::string{ path } };
}

// The destination as a '/'-joined string relative to the project root, which is what the
// assertions below are actually about.
std::string dest_of(envy::vendor_plan const &plan, char const *canonical) {
  auto const *d{ plan.find(envy::pkg_key{ canonical }) };
  REQUIRE(d != nullptr);
  return d->dir.lexically_relative(kRoot).generic_string();
}

envy::vendor_plan resolve(std::vector<envy::vendor_request> const &reqs,
                          char const *root = "vendor") {
  return envy::vendor_resolve(reqs,
                              root ? std::optional<std::string>{ root } : std::nullopt,
                              kRoot);
}

}  // namespace

TEST_CASE("vendor_resolve derives the bare name when nothing collides") {
  auto const plan{ resolve({ derived("local.nanocobs@r3"), derived("arm.gcc@r2") }) };
  CHECK(dest_of(plan, "local.nanocobs@r3") == "vendor/nanocobs");
  CHECK(dest_of(plan, "arm.gcc@r2") == "vendor/gcc");
  CHECK_FALSE(plan.find(envy::pkg_key{ "local.nanocobs@r3" })->overridden);
}

TEST_CASE("vendor_resolve escalates to the namespace only for the colliding group") {
  auto const plan{ resolve({ derived("local.nanocobs@r3"),
                             derived("acme.nanocobs@r3"),
                             derived("arm.gcc@r2") }) };
  CHECK(dest_of(plan, "local.nanocobs@r3") == "vendor/local.nanocobs");
  CHECK(dest_of(plan, "acme.nanocobs@r3") == "vendor/acme.nanocobs");
  CHECK(dest_of(plan, "arm.gcc@r2") == "vendor/gcc");  // untouched by someone else's clash
}

TEST_CASE("vendor_resolve escalates to the revision when the namespace still collides") {
  auto const plan{
    resolve({ derived("local.nanocobs@r3"), derived("local.nanocobs@r4") })
  };
  CHECK(dest_of(plan, "local.nanocobs@r3") == "vendor/local.nanocobs@r3");
  CHECK(dest_of(plan, "local.nanocobs@r4") == "vendor/local.nanocobs@r4");
}

TEST_CASE("vendor_resolve escalates to the options hash for two variants of one spec") {
  // The case the whole escalation ladder exists for: one manifest vendoring a debug and
  // a release build of the same package.
  auto const plan{ resolve({ derived("local.nanocobs@r3{build=\"debug\"}"),
                             derived("local.nanocobs@r3{build=\"release\"}") }) };

  auto const debug{ dest_of(plan, "local.nanocobs@r3{build=\"debug\"}") };
  auto const release{ dest_of(plan, "local.nanocobs@r3{build=\"release\"}") };
  CHECK(debug != release);
  CHECK(debug.starts_with("vendor/local.nanocobs@r3-"));
  CHECK(release.starts_with("vendor/local.nanocobs@r3-"));
  CHECK(debug.size() == std::string{ "vendor/local.nanocobs@r3-" }.size() + 16);
}

TEST_CASE("vendor_resolve is stable under input order") {
  auto const a{ resolve({ derived("local.nanocobs@r3"), derived("acme.nanocobs@r3") }) };
  auto const b{ resolve({ derived("acme.nanocobs@r3"), derived("local.nanocobs@r3") }) };
  CHECK(dest_of(a, "local.nanocobs@r3") == dest_of(b, "local.nanocobs@r3"));
  CHECK(dest_of(a, "acme.nanocobs@r3") == dest_of(b, "acme.nanocobs@r3"));
}

TEST_CASE("vendor_resolve escalates again when raising a group creates a new collision") {
  // "x.local.tool@r1" has name "local.tool", exactly where escalating the two `tool`
  // packages lands one of them. A single pass would leave that collision unresolved.
  auto const plan{ resolve({ derived("local.tool@r1"),
                             derived("acme.tool@r1"),
                             derived("x.local.tool@r1") }) };
  CHECK(dest_of(plan, "local.tool@r1") == "vendor/local.tool@r1");  // raised twice
  CHECK(dest_of(plan, "x.local.tool@r1") == "vendor/x.local.tool");
  CHECK(dest_of(plan, "acme.tool@r1") == "vendor/acme.tool");
}

TEST_CASE("vendor_resolve places an override exactly where it says") {
  auto const plan{ resolve({ overridden("fi.armgcc@r1", "tools/armgcc"),
                             derived("local.nanocobs@r3") }) };
  CHECK(dest_of(plan, "fi.armgcc@r1") == "tools/armgcc");
  CHECK(plan.find(envy::pkg_key{ "fi.armgcc@r1" })->overridden);
  CHECK(dest_of(plan, "local.nanocobs@r3") == "vendor/nanocobs");
}

TEST_CASE("vendor_resolve needs no VENDOR_ROOT when every entry overrides") {
  auto const plan{ envy::vendor_resolve({ overridden("fi.armgcc@r1", "tools/armgcc") },
                                        std::nullopt,
                                        kRoot) };
  CHECK(dest_of(plan, "fi.armgcc@r1") == "tools/armgcc");
}

TEST_CASE("vendor_resolve rejects a derived name with no VENDOR_ROOT") {
  try {
    envy::vendor_resolve({ derived("local.nanocobs@r3") }, std::nullopt, kRoot);
    FAIL("expected a VENDOR_ROOT error");
  } catch (std::runtime_error const &e) {
    std::string const msg{ e.what() };
    CHECK(msg.find("local.nanocobs@r3") != std::string::npos);
    CHECK(msg.find("VENDOR_ROOT") != std::string::npos);
  }
}

TEST_CASE("vendor_resolve rejects two overrides naming one directory") {
  try {
    resolve({ overridden("a.one@r1", "shared/dir"),
              overridden("b.two@r1", "shared/dir") });
    FAIL("expected a collision error");
  } catch (std::runtime_error const &e) {
    std::string const msg{ e.what() };
    CHECK(msg.find("a.one@r1") != std::string::npos);
    CHECK(msg.find("b.two@r1") != std::string::npos);
    CHECK(msg.find("collision") != std::string::npos);
  }
}

TEST_CASE("vendor_resolve steps a derived name aside for an override that claims it") {
  // An override is a fixed point, so the escalation ladder resolves this rather than
  // failing: the author asked for that exact directory and gets it.
  auto const plan{ resolve({ derived("local.nanocobs@r3"),
                             overridden("b.two@r1", "vendor/nanocobs") }) };
  CHECK(dest_of(plan, "b.two@r1") == "vendor/nanocobs");
  CHECK(dest_of(plan, "local.nanocobs@r3") == "vendor/local.nanocobs");
}

TEST_CASE("vendor_resolve rejects nested destinations in either order") {
  for (auto const &reqs : { std::vector<envy::vendor_request>{
                                overridden("a.one@r1", "deps"),
                                overridden("b.two@r1", "deps/inner") },
                            std::vector<envy::vendor_request>{
                                overridden("b.two@r1", "deps/inner"),
                                overridden("a.one@r1", "deps") } }) {
    try {
      resolve(reqs);
      FAIL("expected a nesting error");
    } catch (std::runtime_error const &e) {
      std::string const msg{ e.what() };
      CHECK(msg.find("nested") != std::string::npos);
      CHECK(msg.find("a.one@r1") != std::string::npos);
      CHECK(msg.find("b.two@r1") != std::string::npos);
    }
  }
}

TEST_CASE("vendor_resolve allows sibling directories sharing a name prefix") {
  // "deps/lib" must not read as the parent of "deps/lib-extra": nesting is a component
  // relationship, not a string prefix.
  auto const plan{ resolve({ overridden("a.one@r1", "deps/lib"),
                             overridden("b.two@r1", "deps/lib-extra") }) };
  CHECK(dest_of(plan, "a.one@r1") == "deps/lib");
  CHECK(dest_of(plan, "b.two@r1") == "deps/lib-extra");
}

TEST_CASE("vendor_resolve rejects paths that escape or are not project-relative") {
  for (auto const *bad : { "../outside", "/abs/path", "", "a/../../b", "~/home" }) {
    CHECK_THROWS_AS(resolve({ overridden("a.one@r1", bad) }), std::runtime_error);
    CHECK_THROWS_AS(resolve({ derived("a.one@r1") }, bad), std::runtime_error);
  }
}

TEST_CASE("vendor_resolve on no requests yields an empty plan") {
  auto const plan{ resolve({}) };
  CHECK(plan.empty());
  CHECK(plan.find(envy::pkg_key{ "a.one@r1" }) == nullptr);
}

TEST_CASE("vendor_parse_selectors splits '!' entries into the exclude list") {
  auto const f{ envy::vendor_parse_selectors(
      { "include/**", "!include/internal/**", "LICENSE" }, "spec 'a.b@r1'") };
  REQUIRE(f.include.size() == 2);
  CHECK(f.include[0] == "include/**");
  CHECK(f.include[1] == "LICENSE");
  REQUIRE(f.exclude.size() == 1);
  CHECK(f.exclude[0] == "include/internal/**");
}

TEST_CASE("vendor_parse_selectors canonicalizes both lists") {
  auto const f{ envy::vendor_parse_selectors({ "./include/", "!src\\gen/" }, "ctx") };
  REQUIRE(f.include.size() == 1);
  CHECK(f.include[0] == "include");
  REQUIRE(f.exclude.size() == 1);
  CHECK(f.exclude[0] == "src/gen");
}

TEST_CASE("vendor_parse_selectors rejects unusable patterns, naming the context") {
  for (auto const *bad : { "[unterminated", "!a**b/c", "..", "/abs", "!" }) {
    try {
      envy::vendor_parse_selectors({ bad }, "spec 'a.b@r1'");
      FAIL("expected rejection of: " << bad);
    } catch (std::runtime_error const &e) {
      CHECK(std::string{ e.what() }.find("spec 'a.b@r1'") != std::string::npos);
    }
  }
}

TEST_CASE("vendor_parse_selectors on an empty list selects everything") {
  auto const f{ envy::vendor_parse_selectors({}, "ctx") };
  CHECK(f.empty());
  CHECK(f.selects("anything/at/all"));
}

TEST_CASE("vendor_filter_key separates include from exclude") {
  // Swapping the lists must change the key, or a spec that flipped a pattern from
  // include to exclude would keep serving the old pristine hash.
  auto const a{ envy::vendor_filter_key({ .include = { "a" }, .exclude = { "b" } }) };
  auto const b{ envy::vendor_filter_key({ .include = { "b" }, .exclude = { "a" } }) };
  CHECK(a != b);
  CHECK(a == envy::vendor_filter_key({ .include = { "a" }, .exclude = { "b" } }));
  CHECK(envy::vendor_filter_key({}) == envy::vendor_filter_key({}));
  CHECK(envy::vendor_filter_key({}) != a);
}
