#include "deploy.h"

#include "doctest.h"
#include "embedded_init_resources.h"
#include "platform.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;

// Every round-trip case below is about a root manifest; '@envy root "false"' opts out of
// the check entirely and gets its own case.
envy::envy_meta root_meta() { return envy::envy_meta{ .root = true }; }

// Read-only fixtures; the same tree manifest_tests walks.
fs::path manifest_fixture_root() {
  auto root{ fs::current_path() / "test_data" / "manifest" };
  if (!fs::exists(root)) {
    root = fs::current_path().parent_path().parent_path() / "test_data" / "manifest";
  }
  return fs::absolute(root);
}

std::string_view embedded_posix_template() {
  return { reinterpret_cast<char const *>(envy::embedded::kProductScriptPosix),
           envy::embedded::kProductScriptPosixSize };
}

std::string_view embedded_windows_template() {
  return { reinterpret_cast<char const *>(envy::embedded::kProductScriptWindows),
           envy::embedded::kProductScriptWindowsSize };
}

}  // namespace

TEST_CASE("deploy: kProductScriptVersion is positive") {
  CHECK(envy::kProductScriptVersion > 0);
}

TEST_CASE("deploy: embedded POSIX template has version baked at build time") {
  std::string_view const tmpl{ embedded_posix_template() };
  std::string const expected_marker{ "schema \"" +
                                     std::to_string(envy::kProductScriptVersion) + "\"" };
  CHECK(tmpl.find(expected_marker) != std::string_view::npos);
  CHECK(tmpl.find("@@ENVY_PRODUCT_SCRIPT_VERSION@@") == std::string_view::npos);
  CHECK(tmpl.find("@@ENVY_VERSION@@") == std::string_view::npos);
  CHECK(tmpl.find("envy-managed") != std::string_view::npos);
  CHECK(tmpl.find("@@PROJECT_ROOT_REL@@") != std::string_view::npos);
}

TEST_CASE("deploy: embedded Windows template has version baked at build time") {
  std::string_view const tmpl{ embedded_windows_template() };
  std::string const expected_marker{ "schema \"" +
                                     std::to_string(envy::kProductScriptVersion) + "\"" };
  CHECK(tmpl.find(expected_marker) != std::string_view::npos);
  CHECK(tmpl.find("@@ENVY_PRODUCT_SCRIPT_VERSION@@") == std::string_view::npos);
  CHECK(tmpl.find("@@ENVY_VERSION@@") == std::string_view::npos);
  CHECK(tmpl.find("envy-managed") != std::string_view::npos);
  CHECK(tmpl.find("@@PROJECT_ROOT_REL@@") != std::string_view::npos);
}

TEST_CASE("deploy: stamp_product_script substitutes product name on POSIX") {
  std::string const stamped{
    envy::deploy_stamp_product_script("foo", envy::platform_id::POSIX, "..")
  };
  CHECK(stamped.find("foo") != std::string::npos);
  CHECK(stamped.find("@@PRODUCT_NAME@@") == std::string::npos);
  CHECK(stamped.find("envy-managed") != std::string::npos);
  CHECK(stamped.find("schema \"") != std::string::npos);
}

TEST_CASE("deploy: stamp_product_script substitutes product name on Windows") {
  std::string const stamped{
    envy::deploy_stamp_product_script("foo", envy::platform_id::WINDOWS, "..")
  };
  CHECK(stamped.find("foo") != std::string::npos);
  CHECK(stamped.find("@@PRODUCT_NAME@@") == std::string::npos);
  CHECK(stamped.find("envy-managed") != std::string::npos);
  CHECK(stamped.find("schema \"") != std::string::npos);
}

TEST_CASE("deploy: stamp_product_script is deterministic") {
  std::string const a{
    envy::deploy_stamp_product_script("foo", envy::platform_id::POSIX, "..")
  };
  std::string const b{
    envy::deploy_stamp_product_script("foo", envy::platform_id::POSIX, "..")
  };
  CHECK(a == b);
}

TEST_CASE("deploy: stamp_product_script differs by product name") {
  std::string const foo{
    envy::deploy_stamp_product_script("foo", envy::platform_id::POSIX, "..")
  };
  std::string const bar{
    envy::deploy_stamp_product_script("bar", envy::platform_id::POSIX, "..")
  };
  CHECK(foo != bar);
  CHECK(foo.find("foo") != std::string::npos);
  CHECK(foo.find("bar") == std::string::npos);
  CHECK(bar.find("bar") != std::string::npos);
  CHECK(bar.find("foo") == std::string::npos);
}

TEST_CASE("deploy: stamp_product_script differs by platform") {
  std::string const posix{
    envy::deploy_stamp_product_script("foo", envy::platform_id::POSIX, "..")
  };
  std::string const win{
    envy::deploy_stamp_product_script("foo", envy::platform_id::WINDOWS, "..")
  };
  CHECK(posix != win);
  CHECK(posix.find("#!/usr/bin/env bash") != std::string::npos);
  CHECK(win.find("@echo off") != std::string::npos);
}

TEST_CASE("deploy: stamp_product_script substitutes the project root hop") {
  for (auto const plat : { envy::platform_id::POSIX, envy::platform_id::WINDOWS }) {
    std::string const stamped{ envy::deploy_stamp_product_script("foo", plat, "../..") };
    CHECK(stamped.find("@@PROJECT_ROOT_REL@@") == std::string::npos);
    CHECK(stamped.find("../..") != std::string::npos);
    // A relative hop, never an absolute path: the bin dir is committed, so the tree gets
    // cloned to a different prefix than the one that deployed it.
    CHECK(stamped.find("ENVY_PROJECT_ROOT") != std::string::npos);
  }
}

TEST_CASE("deploy: stamp_product_script leaves the root unset for an empty hop") {
  // What deploy stamps for an '@envy root "false"' manifest: which project its scripts
  // resolve depends on where the tree is nested, so the caller's value has to stand.
  std::string const posix{
    envy::deploy_stamp_product_script("foo", envy::platform_id::POSIX, "")
  };
  CHECK(posix.find("@@PROJECT_ROOT_REL@@") == std::string::npos);
  CHECK(posix.find("ENVY_PROJECT_ROOT_HOP=\"\"") != std::string::npos);

  std::string const win{
    envy::deploy_stamp_product_script("foo", envy::platform_id::WINDOWS, "")
  };
  CHECK(win.find("@@PROJECT_ROOT_REL@@") == std::string::npos);
  CHECK(win.find("set \"ENVY_PROJECT_ROOT_HOP=\"") != std::string::npos);
}

TEST_CASE("deploy: stamp_product_script differs by project root hop") {
  std::string const up{
    envy::deploy_stamp_product_script("foo", envy::platform_id::POSIX, "..")
  };
  std::string const here{
    envy::deploy_stamp_product_script("foo", envy::platform_id::POSIX, ".")
  };
  CHECK(up != here);
}

TEST_CASE("deploy: verify_bin_dir accepts a bin dir under its own manifest") {
  auto const root{ manifest_fixture_root() };
  REQUIRE(fs::exists(root / "repo" / "sibling"));
  CHECK_NOTHROW(envy::deploy_verify_bin_dir(root / "repo" / "sibling",
                                            root / "repo" / "envy.lua",
                                            root_meta()));
}

TEST_CASE("deploy: verify_bin_dir accepts the manifest's own directory as the bin dir") {
  // '@envy bin "."' -- the walk starts on the manifest itself.
  auto const root{ manifest_fixture_root() };
  CHECK_NOTHROW(
      envy::deploy_verify_bin_dir(root / "repo", root / "repo" / "envy.lua", root_meta()));
}

TEST_CASE("deploy: verify_bin_dir rejects a bin dir that resolves another manifest") {
  auto const root{ manifest_fixture_root() };
  CHECK_THROWS_WITH_AS(envy::deploy_verify_bin_dir(root / "repo" / "sibling",
                                                   root / "cache_directive" / "envy.lua",
                                                   root_meta()),
                       doctest::Contains("cache_directive"),
                       std::runtime_error);
}

TEST_CASE("deploy: verify_bin_dir only warns for a manifest discovery cannot see") {
  // '--manifest ci.lua' is a variant build: no bin dir placement can make an upward walk
  // for 'envy.lua' land on it, so refusing would break the workflow outright.
  auto const root{ manifest_fixture_root() };
  CHECK_NOTHROW(envy::deploy_verify_bin_dir(root / "repo" / "sibling",
                                            root / "repo" / "variant.lua",
                                            root_meta()));
}

TEST_CASE("deploy: verify_bin_dir names both manifests in the refusal") {
  auto const root{ manifest_fixture_root() };
  CHECK_THROWS_WITH_AS(envy::deploy_verify_bin_dir(root / "non_git_dir" / "deeply",
                                                   root / "repo" / "envy.lua",
                                                   root_meta()),
                       doctest::Contains("non_git_dir"),
                       std::runtime_error);
}

TEST_CASE("deploy: verify_bin_dir skips the round trip for '@envy root \"false\"'") {
  // The layout that refuses for a root manifest is the declared design for a non-root
  // one: a nested tree deferring to its parent, whose bin dir a superproject checkout
  // still has to be able to restamp in place.
  auto const root{ manifest_fixture_root() };
  envy::envy_meta const non_root{ .root = false };
  CHECK_NOTHROW(envy::deploy_verify_bin_dir(root / "non_git_dir" / "deeply",
                                            root / "repo" / "envy.lua",
                                            non_root));
}
