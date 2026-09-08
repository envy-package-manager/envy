#include "lua_envy_dep_util.h"

#include "engine.h"
#include "pkg.h"
#include "pkg_cfg.h"
#include "pkg_key.h"

#include "doctest.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace envy {

namespace {

std::unique_ptr<pkg> make_pkg(std::string identity) {
  pkg_cfg *cfg{ pkg_cfg::pool()->emplace(std::move(identity),
                                         pkg_cfg::weak_ref{},
                                         "{}",
                                         std::nullopt,
                                         nullptr,
                                         nullptr,
                                         std::vector<pkg_cfg *>{},
                                         std::nullopt,
                                         std::filesystem::path{}) };

  return std::unique_ptr<pkg>(new pkg{ .key = pkg_key(*cfg),
                                       .cfg = cfg,
                                       .cache_ptr = nullptr,
                                       .eng = nullptr,
                                       .tui_section = {},
                                       .lua = nullptr,
                                       .lock = nullptr,
                                       .canonical_identity_hash = {},
                                       .pkg_path = {},
                                       .spec_file_path = std::nullopt,
                                       .result_hash = {},
                                       .type = pkg_type::CACHE_MANAGED });
}

// The engine keys every edge by the dependency's bare identity; mirror that here so
// the lookup under test sees exactly the map it sees in production.
void link(pkg *from, pkg *to, pkg_phase needed_by) {
  from->dependencies.emplace(to->cfg->identity,
                             pkg::dependency_info{ .p = to, .needed_by = needed_by });
}

}  // namespace

TEST_CASE("find_direct_dependency: exact identity matches its own edge") {
  auto const consumer{ make_pkg("local.consumer@v1") };
  auto const dep{ make_pkg("local.python@r4") };
  link(consumer.get(), dep.get(), pkg_phase::pkg_stage);

  auto const hit{ find_direct_dependency(consumer.get(), "local.python@r4") };
  REQUIRE(hit.has_value());
  CHECK(hit->p == dep.get());
  CHECK(hit->identity == "local.python@r4");
  CHECK(hit->needed_by == pkg_phase::pkg_stage);
}

TEST_CASE("find_direct_dependency: fuzzy queries match a direct edge") {
  auto const consumer{ make_pkg("local.consumer@v1") };
  auto const dep{ make_pkg("acme.toolchain-helpers@v1") };
  link(consumer.get(), dep.get(), pkg_phase::pkg_build);

  for (char const *query : { "toolchain-helpers",
                             "acme.toolchain-helpers",
                             "toolchain-helpers@v1",
                             "acme.toolchain-helpers@v1" }) {
    auto const hit{ find_direct_dependency(consumer.get(), query) };
    REQUIRE_MESSAGE(hit.has_value(), query);
    CHECK(hit->p == dep.get());
  }
}

TEST_CASE("find_direct_dependency: no edge, no match") {
  auto const consumer{ make_pkg("local.consumer@v1") };
  auto const dep{ make_pkg("local.python@r4") };
  link(consumer.get(), dep.get(), pkg_phase::pkg_build);

  CHECK_FALSE(find_direct_dependency(consumer.get(), "ruby").has_value());
  CHECK_FALSE(find_direct_dependency(consumer.get(), "vendor.python").has_value());
  CHECK_FALSE(find_direct_dependency(consumer.get(), "python@r3").has_value());
}

TEST_CASE("find_direct_dependency: a transitive-only target is not a match") {
  // A -> B -> X. The transitive walk this replaced answered "yes" and then handed
  // back B's identity, so envy.package("X") returned B's installed directory.
  auto const a{ make_pkg("local.a@v1") };
  auto const b{ make_pkg("local.b@v1") };
  auto const x{ make_pkg("local.x@v1") };
  link(a.get(), b.get(), pkg_phase::pkg_build);
  link(b.get(), x.get(), pkg_phase::pkg_build);

  CHECK_FALSE(find_direct_dependency(a.get(), "local.x@v1").has_value());
  CHECK_FALSE(find_direct_dependency(a.get(), "x").has_value());
}

TEST_CASE("find_direct_dependency: earliest needed_by wins among matching edges") {
  // Two option variants of one identity cannot share a map key, but a fuzzy query
  // can still match several distinct identities; the earliest gate is the safe one.
  auto const consumer{ make_pkg("local.consumer@v1") };
  auto const early{ make_pkg("acme.gcc@v1") };
  auto const late{ make_pkg("other.gcc@v2") };
  link(consumer.get(), late.get(), pkg_phase::pkg_install);
  link(consumer.get(), early.get(), pkg_phase::pkg_fetch);

  auto const hit{ find_direct_dependency(consumer.get(), "gcc") };
  REQUIRE(hit.has_value());
  CHECK(hit->p == early.get());
  CHECK(hit->needed_by == pkg_phase::pkg_fetch);
}

}  // namespace envy
