#include "glob.h"

#include "doctest.h"

#include <stdexcept>
#include <string>
#include <vector>

TEST_CASE("glob_is_safe_relative_path rejects escape vectors") {
  CHECK(envy::glob_is_safe_relative_path("a.txt"));
  CHECK(envy::glob_is_safe_relative_path("a/b/c.txt"));
  CHECK(envy::glob_is_safe_relative_path("a/..b/c"));  // ".." substring, not component
  CHECK(envy::glob_is_safe_relative_path("a/b../c"));
  CHECK(envy::glob_is_safe_relative_path("..a/b"));
  CHECK(envy::glob_is_safe_relative_path("a..b"));

  CHECK_FALSE(envy::glob_is_safe_relative_path(nullptr));
  CHECK_FALSE(envy::glob_is_safe_relative_path(""));
  CHECK_FALSE(envy::glob_is_safe_relative_path("/abs/path"));
  CHECK_FALSE(envy::glob_is_safe_relative_path("\\abs\\path"));
  CHECK_FALSE(envy::glob_is_safe_relative_path(".."));
  CHECK_FALSE(envy::glob_is_safe_relative_path("../x"));
  CHECK_FALSE(envy::glob_is_safe_relative_path("a/../../x"));
  CHECK_FALSE(envy::glob_is_safe_relative_path("a/.."));
  CHECK_FALSE(envy::glob_is_safe_relative_path("a\\..\\x"));
#ifdef _WIN32
  CHECK_FALSE(envy::glob_is_safe_relative_path("C:\\evil"));
  CHECK_FALSE(envy::glob_is_safe_relative_path("c:/evil"));
#endif
}

TEST_CASE("glob_canonical_path normalizes separators and decoration") {
  CHECK(envy::glob_canonical_path("bin/clang") == "bin/clang");
  CHECK(envy::glob_canonical_path("bin\\clang") == "bin/clang");
  CHECK(envy::glob_canonical_path("./bin/clang") == "bin/clang");
  CHECK(envy::glob_canonical_path("././bin") == "bin");
  CHECK(envy::glob_canonical_path("bin/") == "bin");
  CHECK(envy::glob_canonical_path("bin///") == "bin");
  CHECK(envy::glob_canonical_path(".\\bin\\") == "bin");
  CHECK(envy::glob_canonical_path("").empty());
  CHECK(envy::glob_canonical_path("./").empty());

  // Repeated separators collapse, so a canonical path never has an empty component.
  CHECK(envy::glob_canonical_path("a//b") == "a/b");
  CHECK(envy::glob_canonical_path("a///b////c") == "a/b/c");
  CHECK(envy::glob_canonical_path("a\\\\b") == "a/b");
  CHECK(envy::glob_canonical_path("a/\\b") == "a/b");
  CHECK(envy::glob_canonical_path(".//a//") == "a");
}

TEST_CASE("glob_match: literal entries keep exact-or-subtree semantics") {
  CHECK(envy::glob_match("bin/clang-format", "bin/clang-format"));
  CHECK(envy::glob_match("bin", "bin"));
  CHECK(envy::glob_match("bin", "bin/clang"));
  CHECK(envy::glob_match("lib/clang", "lib/clang/19/include/stdatomic.h"));

  CHECK_FALSE(envy::glob_match("bin/clang-format", "bin/clang-format-diff"));
  CHECK_FALSE(envy::glob_match("bin/clang-format", "bin"));
  CHECK_FALSE(envy::glob_match("bin", "binary"));
  CHECK_FALSE(envy::glob_match("lib/clang", "lib/clangd"));
  CHECK_FALSE(envy::glob_match("lib/clang", "usr/lib/clang"));
  CHECK_FALSE(envy::glob_match("a/b/c", "a/b"));
}

TEST_CASE("glob_match: '*' matches within one component only") {
  CHECK(envy::glob_match("bin/clang-*", "bin/clang-format"));
  CHECK(envy::glob_match("bin/clang-*", "bin/clang-tidy"));
  CHECK(envy::glob_match("bin/*", "bin/clangd"));
  CHECK(envy::glob_match("*", "bin"));
  CHECK(envy::glob_match("*/clangd", "bin/clangd"));
  CHECK(envy::glob_match("*.h", "stdatomic.h"));
  CHECK(envy::glob_match("lib*", "libclang.so"));
  CHECK(envy::glob_match("cl*d", "clangd"));
  CHECK(envy::glob_match("*clang*", "libclang.so"));
  CHECK(envy::glob_match("clang*", "clang"));  // '*' matches an empty run
  CHECK(envy::glob_match("**", "a/b/c"));
  CHECK_FALSE(envy::glob_match("*", ""));  // the archive root is unselectable

  CHECK_FALSE(envy::glob_match("bin/clang-*", "bin/clang"));
  CHECK_FALSE(envy::glob_match("bin/*.h", "bin/sub/x.h"));  // no '/' crossing
  CHECK_FALSE(envy::glob_match("a*c", "ab/c"));
  CHECK_FALSE(envy::glob_match("*.h", "x.hpp"));
  CHECK_FALSE(envy::glob_match("*.h", "h"));
  CHECK_FALSE(envy::glob_match("bin/*", "bin"));  // '*' needs a component
}

TEST_CASE("glob_match: '?' matches exactly one character") {
  CHECK(envy::glob_match("file?.txt", "file1.txt"));
  CHECK(envy::glob_match("subdir?", "subdir2"));
  CHECK(envy::glob_match("??", "ab"));
  CHECK(envy::glob_match("a?c/d", "abc/d"));

  CHECK_FALSE(envy::glob_match("file?.txt", "file.txt"));
  CHECK_FALSE(envy::glob_match("file?.txt", "file12.txt"));
  CHECK_FALSE(envy::glob_match("a?c", "a/c"));  // '?' never matches '/'
  CHECK_FALSE(envy::glob_match("??", "a"));
}

TEST_CASE("glob_match: '[...]' character classes") {
  CHECK(envy::glob_match("file[123].txt", "file2.txt"));
  CHECK(envy::glob_match("file[0-9].txt", "file7.txt"));
  CHECK(envy::glob_match("[a-z]*", "clangd"));
  CHECK(envy::glob_match("[a-cx-z]", "y"));
  CHECK(envy::glob_match("[!0-9]*", "clangd"));
  CHECK(envy::glob_match("[^0-9]*", "clangd"));
  CHECK(envy::glob_match("[a-]", "-"));
  CHECK(envy::glob_match("[-a]", "-"));
  CHECK(envy::glob_match("[]]", "]"));
  CHECK(envy::glob_match("[*]", "*"));  // metacharacters go literal in a class
  CHECK(envy::glob_match("[?]", "?"));
  CHECK(envy::glob_match("v[0-9].[0-9]", "v1.2"));
  CHECK(envy::glob_match("*[0-9]", "file1"));  // class after a backtracking '*'

  CHECK_FALSE(envy::glob_match("file[123].txt", "file4.txt"));
  CHECK_FALSE(envy::glob_match("file[0-9].txt", "filex.txt"));
  CHECK_FALSE(envy::glob_match("[!0-9]*", "1clangd"));
  CHECK_FALSE(envy::glob_match("[^0-9]*", "1clangd"));
  CHECK_FALSE(envy::glob_match("[*]", "x"));
  CHECK_FALSE(envy::glob_match("a[/]b", "a/b"));  // classes never span components
  CHECK_FALSE(envy::glob_match("[a-c]", "d"));
}

TEST_CASE("glob_match: '**' spans components") {
  CHECK(envy::glob_match("**/file4.txt", "file4.txt"));
  CHECK(envy::glob_match("**/file4.txt", "root/file4.txt"));
  CHECK(envy::glob_match("**/file4.txt", "root/a/b/c/file4.txt"));
  CHECK(envy::glob_match("root/**", "root"));
  CHECK(envy::glob_match("root/**", "root/a/b"));
  CHECK(envy::glob_match("a/**/b", "a/b"));
  CHECK(envy::glob_match("a/**/b", "a/x/b"));
  CHECK(envy::glob_match("a/**/b", "a/x/y/b"));
  CHECK(envy::glob_match("**", "anything/at/all"));
  CHECK(envy::glob_match("lib/**/include/*.h", "lib/clang/20/include/atomic.h"));
  CHECK(envy::glob_match("**/x/**/y", "x/y"));
  CHECK(envy::glob_match("**/x/**/y", "a/b/x/c/d/y"));
  CHECK(envy::glob_match("**/b/**/c", "b/x/b/y/c"));
  CHECK(envy::glob_match("**/*.h", "a/b/c.h"));

  CHECK_FALSE(envy::glob_match("a/**/b", "a/x/y/c"));
  CHECK_FALSE(envy::glob_match("a/**", "ab/c"));
  CHECK_FALSE(envy::glob_match("**/file4.txt", "root/file4.txt.bak"));
  CHECK_FALSE(envy::glob_match("**/x/**/y", "x/z"));
  CHECK_FALSE(envy::glob_match("lib/**/include/*.h", "lib/clang/include/x.hpp"));
}

TEST_CASE("glob_match: pathological patterns terminate without matching") {
  // One saved star per level keeps this linear; a recursive matcher would blow up here.
  CHECK_FALSE(
      envy::glob_match("a*a*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
  CHECK(envy::glob_match("a*a*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab"));
  CHECK_FALSE(envy::glob_match("**/**/**/x", "a/b/c/d/e/f/g/h/i/j"));
  CHECK(envy::glob_match("**/**/**/x", "a/b/c/d/e/f/g/h/i/x"));
  CHECK_FALSE(envy::glob_match("*?*?*?*?*z", "aaaaaaaaaaaaaaaaaaaa"));
}

TEST_CASE("glob_match: matching is case sensitive on every platform") {
  CHECK_FALSE(envy::glob_match("bin/Clang", "bin/clang"));
  CHECK_FALSE(envy::glob_match("*.H", "x.h"));
  CHECK_FALSE(envy::glob_match("[a-z]", "A"));
  CHECK(envy::glob_match("[A-Za-z]", "A"));
}

TEST_CASE("glob_normalize_selectors canonicalizes and rejects unusable entries") {
  auto const ok{
    envy::glob_normalize_selectors({ "./bin/", "lib\\clang" }, "ctx", "sel")
  };
  REQUIRE(ok.size() == 2);
  CHECK(ok[0] == "bin");
  CHECK(ok[1] == "lib/clang");

  for (auto const &bad : { "", ".", "/abs", "../escape", "bin/../../escape" }) {
    try {
      envy::glob_normalize_selectors({ bad }, "ctx", "sel");
      FAIL("Expected rejection of selector: " << bad);
    } catch (std::runtime_error const &e) {
      CHECK(std::string{ e.what() }.find("ctx: sel") != std::string::npos);
    }
  }
}

TEST_CASE("glob_normalize_selectors accepts well-formed glob patterns") {
  auto const ok{ envy::glob_normalize_selectors(
      { "bin/clang-*", "lib/**/include/*.h", "[a-z]?[!0-9]", "**", "a/**/b", "[]]" },
      "ctx",
      "sel") };
  REQUIRE(ok.size() == 6);
  CHECK(ok[0] == "bin/clang-*");
  CHECK(ok[1] == "lib/**/include/*.h");
  CHECK(ok[2] == "[a-z]?[!0-9]");
  CHECK(ok[3] == "**");
  CHECK(ok[4] == "a/**/b");
  CHECK(ok[5] == "[]]");
}

TEST_CASE("glob_normalize_selectors rejects malformed glob patterns") {
  auto const expect_reject{ [](char const *bad, char const *needle) {
    try {
      envy::glob_normalize_selectors({ bad }, "ctx", "sel");
      FAIL("Expected rejection of glob pattern: " << bad);
    } catch (std::runtime_error const &e) {
      std::string const msg{ e.what() };
      CHECK(msg.find("ctx: sel") != std::string::npos);
      CHECK(msg.find(bad) != std::string::npos);
      CHECK(msg.find(needle) != std::string::npos);
    }
  } };

  for (auto const *bad : { "[", "[abc", "bin/[a-z", "[!abc", "[]", "a[b/c]d" }) {
    expect_reject(bad, "unterminated '['");
  }
  for (auto const *bad : { "a**", "**b", "a**b", "bin/x**/y", "**/a**" }) {
    expect_reject(bad, "'**' a path component of its own");
  }
}

TEST_CASE("glob_selectors_match flags every matching pattern") {
  std::vector<std::string> const selectors{ "bin/*", "**/*.h", "share" };
  std::vector<bool> matched;

  CHECK(envy::glob_selectors_match(selectors, "bin/clang", matched));
  CHECK(envy::glob_selectors_match(selectors, "lib/clang/20/x.h", matched));
  CHECK_FALSE(envy::glob_selectors_match(selectors, "docs/readme.md", matched));

  auto const unmatched{ envy::glob_unmatched_selectors(selectors, matched) };
  REQUIRE(unmatched.size() == 1);
  CHECK(unmatched[0] == "share");

  // One entry satisfying two patterns marks both.
  std::vector<std::string> const overlap{ "bin/*", "bin/clang" };
  std::vector<bool> overlap_matched;
  CHECK(envy::glob_selectors_match(overlap, "bin/clang", overlap_matched));
  CHECK(envy::glob_unmatched_selectors(overlap, overlap_matched).empty());
}

TEST_CASE("glob_selectors_match matches files exactly and directories by subtree") {
  std::vector<std::string> const selectors{ "bin/clang-format", "lib/clang" };
  std::vector<bool> matched;

  CHECK(envy::glob_selectors_match(selectors, "bin/clang-format", matched));
  CHECK(envy::glob_selectors_match(selectors, "lib/clang", matched));
  CHECK(envy::glob_selectors_match(selectors,
                                      "lib/clang/19/include/stdatomic.h",
                                      matched));

  // Prefix-of-a-name is not a subtree; neither is a parent of a selected entry.
  CHECK_FALSE(envy::glob_selectors_match(selectors, "bin/clang-format-diff", matched));
  CHECK_FALSE(envy::glob_selectors_match(selectors, "bin", matched));
  CHECK_FALSE(envy::glob_selectors_match(selectors, "lib/clangd", matched));
  CHECK_FALSE(envy::glob_selectors_match(selectors, "share/man", matched));

  CHECK(envy::glob_unmatched_selectors(selectors, matched).empty());
}

TEST_CASE("glob_selectors_match flags every matching entry, not just the first") {
  // Overlapping entries must all count as matched, else a redundant-but-valid selector
  // list would look unsatisfied.
  std::vector<std::string> const selectors{ "bin", "bin/clang", "share" };
  std::vector<bool> matched;

  CHECK(envy::glob_selectors_match(selectors, "bin/clang", matched));

  auto const unmatched{ envy::glob_unmatched_selectors(selectors, matched) };
  REQUIRE(unmatched.size() == 1);
  CHECK(unmatched[0] == "share");
}

TEST_CASE("glob_parse_filter splits '!' entries into the exclude list") {
  auto const f{ envy::glob_parse_filter(
      { "include/**", "!include/internal/**", "LICENSE" }, "ctx", "sel") };
  REQUIRE(f.include.size() == 2);
  CHECK(f.include[0] == "include/**");
  CHECK(f.include[1] == "LICENSE");
  REQUIRE(f.exclude.size() == 1);
  CHECK(f.exclude[0] == "include/internal/**");
  CHECK_FALSE(f.empty());
}

TEST_CASE("glob_parse_filter canonicalizes both halves") {
  auto const f{ envy::glob_parse_filter({ "./include/", "!src\\gen/" }, "ctx", "sel") };
  REQUIRE(f.include.size() == 1);
  CHECK(f.include[0] == "include");
  REQUIRE(f.exclude.size() == 1);
  CHECK(f.exclude[0] == "src/gen");
}

TEST_CASE("glob_parse_filter rejects unusable patterns on either side") {
  for (auto const *bad : { "[unterminated", "!a**b/c", "..", "/abs", "!", "!../escape" }) {
    try {
      envy::glob_parse_filter({ bad }, "ctx", "sel");
      FAIL("expected rejection of: " << bad);
    } catch (std::runtime_error const &e) {
      CHECK(std::string{ e.what() }.find("ctx") != std::string::npos);
    }
  }
}

TEST_CASE("glob_filter with no patterns selects everything") {
  envy::glob_filter const f;
  CHECK(f.empty());
  CHECK(f.selects("anything/at/all"));
  CHECK(f.selects("x"));
}

TEST_CASE("glob_filter include list is a whitelist") {
  envy::glob_filter const f{ .include = { "bin/**", "LICENSE" } };
  CHECK(f.selects("bin/clang"));
  CHECK(f.selects("LICENSE"));
  CHECK_FALSE(f.selects("lib/libc.a"));
  CHECK_FALSE(f.selects("LICENSE.md"));
}

TEST_CASE("glob_filter exclude list subtracts, and beats include") {
  envy::glob_filter const f{ .include = { "src/**" }, .exclude = { "src/**/*_test.c" } };
  CHECK(f.selects("src/lib.c"));
  CHECK(f.selects("src/deep/lib.c"));
  CHECK_FALSE(f.selects("src/lib_test.c"));
  CHECK_FALSE(f.selects("src/deep/lib_test.c"));
  CHECK_FALSE(f.selects("docs/readme.md"));
}

TEST_CASE("glob_filter exclude alone subtracts from everything") {
  envy::glob_filter const f{ .exclude = { "build" } };
  CHECK(f.selects("src/lib.c"));
  CHECK_FALSE(f.selects("build"));
  CHECK_FALSE(f.selects("build/out.o"));  // a literal names its whole subtree
}

TEST_CASE("glob_filter flags which include patterns matched") {
  envy::glob_filter const f{ .include = { "bin/**", "share/**" },
                             .exclude = { "bin/secret" } };
  std::vector<bool> matched;

  CHECK(f.selects("bin/clang", matched));
  CHECK_FALSE(f.selects("bin/secret", matched));  // excluded, but the include did match
  CHECK_FALSE(f.selects("lib/libc.a", matched));

  // Only "share/**" never named anything, which is the one an author would want told.
  auto const unmatched{ envy::glob_unmatched_selectors(f.include, matched) };
  REQUIRE(unmatched.size() == 1);
  CHECK(unmatched[0] == "share/**");
}

TEST_CASE("glob_filter both selects() overloads agree") {
  envy::glob_filter const f{ .include = { "a/**", "b/*.h" }, .exclude = { "a/skip" } };
  std::vector<bool> matched;
  for (auto const *path :
       { "a/x", "a/skip", "a/skip/deep", "b/x.h", "b/x.c", "c/y", "a", "b" }) {
    CHECK(f.selects(path) == f.selects(path, matched));
  }
}
