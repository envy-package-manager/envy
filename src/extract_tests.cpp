#include "extract.h"

#include "doctest.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

std::filesystem::path make_temp_dir() {
  static std::mt19937_64 rng{ std::random_device{}() };
  auto suffix{ std::to_string(rng()) };
  auto dir{ std::filesystem::temp_directory_path() /
            std::filesystem::path("envy-extract-test-" + suffix) };
  std::filesystem::create_directories(dir);
  return dir;
}

std::vector<std::string> collect_files_recursive(std::filesystem::path const &root) {
  std::vector<std::string> files;
  for (auto const &entry : std::filesystem::recursive_directory_iterator(root)) {
    if (entry.is_regular_file()) {
      auto const rel{ std::filesystem::relative(entry.path(), root) };
      files.push_back(rel.generic_string());
    }
  }
  std::ranges::sort(files);
  return files;
}

std::uint64_t sum_file_sizes(std::filesystem::path const &root) {
  std::uint64_t total{ 0 };
  for (auto const &entry : std::filesystem::recursive_directory_iterator(root)) {
    if (entry.is_regular_file()) { total += std::filesystem::file_size(entry.path()); }
  }
  return total;
}

}  // namespace

TEST_CASE("extract with strip_components=0 preserves structure") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  envy::extract_options opts{ .strip_components = 0 };
  auto const count{ envy::extract(archive, dest, opts) };

  CHECK(count == 5);  // 5 regular files

  auto const files{ collect_files_recursive(dest) };
  CHECK(files.size() == 5);
  CHECK(files[0] == "root/file1.txt");
  CHECK(files[1] == "root/file2.txt");
  CHECK(files[2] == "root/subdir1/file3.txt");
  CHECK(files[3] == "root/subdir1/nested/file4.txt");
  CHECK(files[4] == "root/subdir2/file5.txt");

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract with strip_components=1 removes top-level directory") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  envy::extract_options opts{ .strip_components = 1 };
  auto const count{ envy::extract(archive, dest, opts) };

  CHECK(count == 5);

  auto const files{ collect_files_recursive(dest) };
  CHECK(files.size() == 5);
  CHECK(files[0] == "file1.txt");
  CHECK(files[1] == "file2.txt");
  CHECK(files[2] == "subdir1/file3.txt");
  CHECK(files[3] == "subdir1/nested/file4.txt");
  CHECK(files[4] == "subdir2/file5.txt");

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract with strip_components=2 removes two levels") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  envy::extract_options opts{ .strip_components = 2 };
  auto const count{ envy::extract(archive, dest, opts) };

  // Only files at least 2 levels deep are extracted
  CHECK(count == 3);

  auto const files{ collect_files_recursive(dest) };
  CHECK(files.size() == 3);
  CHECK(files[0] == "file3.txt");
  CHECK(files[1] == "file5.txt");
  CHECK(files[2] == "nested/file4.txt");

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract with strip_components=3 extracts deeply nested only") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  envy::extract_options opts{ .strip_components = 3 };
  auto const count{ envy::extract(archive, dest, opts) };

  // Only file4.txt is at least 3 levels deep (root/subdir1/nested/file4.txt)
  CHECK(count == 1);

  auto const files{ collect_files_recursive(dest) };
  CHECK(files.size() == 1);
  CHECK(files[0] == "file4.txt");

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract with strip_components too large throws error") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  envy::extract_options opts{ .strip_components = 10 };

  try {
    envy::extract(archive, dest, opts);
    FAIL("Expected exception to be thrown");
  } catch (std::runtime_error const &e) {
    std::string const msg{ e.what() };
    CHECK(msg.find("test.tar.gz") != std::string::npos);
    CHECK(msg.find("strip=10") != std::string::npos);
  }

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract flat archive with strip=0 succeeds") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/flat.tar.gz") };

  envy::extract_options opts{ .strip_components = 0 };
  auto const count{ envy::extract(archive, dest, opts) };

  CHECK(count == 3);

  auto const files{ collect_files_recursive(dest) };
  CHECK(files.size() == 3);
  CHECK(files[0] == "file1.txt");
  CHECK(files[1] == "file2.txt");
  CHECK(files[2] == "file3.txt");

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract flat archive with strip=1 throws error") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/flat.tar.gz") };

  envy::extract_options opts{ .strip_components = 1 };

  try {
    envy::extract(archive, dest, opts);
    FAIL("Expected exception - flat archive cannot be stripped");
  } catch (std::runtime_error const &e) {
    std::string const msg{ e.what() };
    CHECK(msg.find("flat.tar.gz") != std::string::npos);
    CHECK(msg.find("strip=1") != std::string::npos);
    CHECK(msg.find("0 files extracted") != std::string::npos);
  }

  std::filesystem::remove_all(dest);
}

TEST_CASE("compute_extract_totals counts uncompressed archive bytes and plain files") {
  auto const fetch_dir{ make_temp_dir() };
  auto const archive_src{ std::filesystem::path("test_data/archives/test.tar.gz") };
  auto const archive_dest{ fetch_dir / "test.tar.gz" };
  std::filesystem::copy_file(archive_src, archive_dest);

  // Add a plain file (11 bytes)
  auto const plain{ fetch_dir / "plain.txt" };
  {
    std::ofstream out{ plain, std::ios::binary };
    out << "hello world";
  }

  // Ground truth: extract archive and sum uncompressed bytes
  auto const dest{ make_temp_dir() };
  envy::extract_options opts{ .strip_components = 0 };
  auto const files_in_archive{ envy::extract(archive_dest, dest, opts) };
  REQUIRE(files_in_archive == 5);
  std::uint64_t const archive_bytes{ sum_file_sizes(dest) };

  envy::extract_totals const totals{ envy::compute_extract_totals(fetch_dir) };

  CHECK(totals.files == 6);  // 5 from archive + 1 plain
  CHECK(totals.bytes == archive_bytes + 11);

  std::filesystem::remove_all(fetch_dir);
  std::filesystem::remove_all(dest);
}

TEST_CASE("compute_archive_totals counts files and bytes in a single archive") {
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  // Ground truth: extract and sum uncompressed regular file sizes
  auto const dest{ make_temp_dir() };
  auto const files_extracted{ envy::extract(archive, dest) };
  REQUIRE(files_extracted == 5);
  std::uint64_t const expected_bytes{ sum_file_sizes(dest) };

  envy::extract_totals const totals{ envy::compute_archive_totals(archive) };
  CHECK(totals.files == 5);
  CHECK(totals.bytes == expected_bytes);

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract different archive formats with strip") {
  auto const test_archives{ std::vector<std::string>{
      "test.tar",
      "test.tar.bz2",
      "test.tar.gz",
      "test.tar.xz",
      "test.tar.zst"
      // Skip test.zip - structure might differ
  } };

  for (auto const &archive_name : test_archives) {
    auto const dest{ make_temp_dir() };
    auto const archive{ std::filesystem::path("test_data/archives") / archive_name };

    envy::extract_options opts{ .strip_components = 1 };
    auto const count{ envy::extract(archive, dest, opts) };

    CHECK(count == 5);

    auto const files{ collect_files_recursive(dest) };
    CHECK(files.size() == 5);

    std::filesystem::remove_all(dest);
  }
}

TEST_CASE("archive_create_tar_zst round-trip with files and directories") {
  auto const source{ make_temp_dir() };
  auto const dest{ make_temp_dir() };

  // Create source tree
  std::filesystem::create_directories(source / "subdir");
  { std::ofstream{ source / "file1.txt" } << "hello"; }
  { std::ofstream{ source / "subdir" / "file2.txt" } << "world"; }

  auto const archive{ make_temp_dir() / "test.tar.zst" };
  auto const files_archived{ envy::archive_create_tar_zst(archive, source, "pkg") };
  CHECK(files_archived == 2);
  CHECK(std::filesystem::exists(archive));
  CHECK(std::filesystem::file_size(archive) > 0);

  // Extract and verify contents under prefix
  envy::extract(archive, dest);

  auto const files{ collect_files_recursive(dest) };
  CHECK(files.size() == 2);
  CHECK(files[0] == "pkg/file1.txt");
  CHECK(files[1] == "pkg/subdir/file2.txt");

  // Verify file contents survived round-trip
  {
    std::ifstream in{ dest / "pkg" / "file1.txt" };
    std::string content{ std::istreambuf_iterator<char>{ in }, {} };
    CHECK(content == "hello");
  }
  {
    std::ifstream in{ dest / "pkg" / "subdir" / "file2.txt" };
    std::string content{ std::istreambuf_iterator<char>{ in }, {} };
    CHECK(content == "world");
  }

  std::filesystem::remove_all(source);
  std::filesystem::remove_all(dest);
  std::filesystem::remove_all(archive.parent_path());
}

#ifndef _WIN32
TEST_CASE("archive_create_tar_zst preserves symlinks") {
  auto const source{ make_temp_dir() };
  auto const dest{ make_temp_dir() };

  // Create source with a symlink
  { std::ofstream{ source / "real.txt" } << "content"; }
  std::filesystem::create_symlink("real.txt", source / "link.txt");

  auto const archive{ make_temp_dir() / "symlink.tar.zst" };
  envy::archive_create_tar_zst(archive, source, "fetch");

  envy::extract(archive, dest);

  auto const link_path{ dest / "fetch" / "link.txt" };
  CHECK(std::filesystem::is_symlink(link_path));
  CHECK(std::filesystem::read_symlink(link_path) == "real.txt");

  std::filesystem::remove_all(source);
  std::filesystem::remove_all(dest);
  std::filesystem::remove_all(archive.parent_path());
}
#endif

TEST_CASE("extract_bare_compressed_output_name strips known suffixes") {
  CHECK(envy::extract_bare_compressed_output_name("hello.txt.gz") ==
        std::filesystem::path{ "hello.txt" });
  CHECK(envy::extract_bare_compressed_output_name("hello.txt.bz2") ==
        std::filesystem::path{ "hello.txt" });
  CHECK(envy::extract_bare_compressed_output_name("hello.txt.xz") ==
        std::filesystem::path{ "hello.txt" });
  CHECK(envy::extract_bare_compressed_output_name("hello.txt.zst") ==
        std::filesystem::path{ "hello.txt" });
  CHECK(envy::extract_bare_compressed_output_name("hello.txt.lzma") ==
        std::filesystem::path{ "hello.txt" });
  CHECK(envy::extract_bare_compressed_output_name("foo.gz") ==
        std::filesystem::path{ "foo" });
  CHECK(envy::extract_bare_compressed_output_name("/abs/path/foo.gz") ==
        std::filesystem::path{ "foo" });
}

TEST_CASE("extract_bare_compressed_output_name rejects tar wrappers and unknowns") {
  CHECK(!envy::extract_bare_compressed_output_name("foo.tar.gz").has_value());
  CHECK(!envy::extract_bare_compressed_output_name("foo.tar.bz2").has_value());
  CHECK(!envy::extract_bare_compressed_output_name("foo.tar.xz").has_value());
  CHECK(!envy::extract_bare_compressed_output_name("foo.tar.zst").has_value());
  CHECK(!envy::extract_bare_compressed_output_name("foo.tar").has_value());
  CHECK(!envy::extract_bare_compressed_output_name("foo.zip").has_value());
  CHECK(!envy::extract_bare_compressed_output_name("foo.bin").has_value());
  CHECK(!envy::extract_bare_compressed_output_name("foo").has_value());
}

TEST_CASE("extract_is_archive_extension recognizes bare compression suffixes") {
  CHECK(envy::extract_is_archive_extension("hello.txt.gz"));
  CHECK(envy::extract_is_archive_extension("hello.txt.bz2"));
  CHECK(envy::extract_is_archive_extension("hello.txt.xz"));
  CHECK(envy::extract_is_archive_extension("hello.txt.zst"));
  CHECK(envy::extract_is_archive_extension("hello.txt.lzma"));
  // tar wrappers continue to match
  CHECK(envy::extract_is_archive_extension("foo.tar.gz"));
  CHECK(envy::extract_is_archive_extension("foo.tgz"));
  // non-archive still rejected
  CHECK(!envy::extract_is_archive_extension("foo.bin"));
  CHECK(!envy::extract_is_archive_extension("foo.txt"));
}

TEST_CASE("extract bare compressed file produces stem-named output") {
  auto const cases{ std::vector<std::string>{
      "hello.txt.gz",
      "hello.txt.bz2",
      "hello.txt.xz",
      "hello.txt.zst",
      "hello.txt.lzma",
  } };

  for (auto const &name : cases) {
    auto const dest{ make_temp_dir() };
    auto const archive{ std::filesystem::path("test_data/archives") / name };

    auto const count{ envy::extract(archive, dest) };
    CHECK(count == 1);

    auto const out{ dest / "hello.txt" };
    CHECK(std::filesystem::exists(out));

    {
      std::ifstream in{ out };
      std::string content{ std::istreambuf_iterator<char>{ in }, {} };
      CHECK(content == "Bare compression test\n");
    }

    std::filesystem::remove_all(dest);
  }
}

TEST_CASE("extract bare compressed with strip_components throws") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/hello.txt.gz") };

  envy::extract_options opts{ .strip_components = 1 };
  try {
    envy::extract(archive, dest, opts);
    FAIL("Expected exception for strip_components on single-stream input");
  } catch (std::runtime_error const &e) {
    std::string const msg{ e.what() };
    CHECK(msg.find("strip_components") != std::string::npos);
    CHECK(msg.find("hello.txt.gz") != std::string::npos);
  }

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract corrupt .gz throws") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/corrupt.gz") };

  try {
    envy::extract(archive, dest);
    FAIL("Expected exception for corrupt .gz");
  } catch (std::runtime_error const &e) {
    // libarchive surfaces a decompression error; just confirm it threw.
    CHECK(std::string{ e.what() }.size() > 0);
  }

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract unrecognized suffix is not silently raw-decoded") {
  // Regression guard: format_raw must NOT be enabled for unknown suffixes,
  // so a random binary file should error out rather than "extract" as a copy.
  auto const dest{ make_temp_dir() };
  auto const fake{ make_temp_dir() / "garbage.bin" };
  std::ofstream{ fake, std::ios::binary } << "not an archive of any kind";

  try {
    envy::extract(fake, dest);
    FAIL("Expected exception for unrecognized binary input");
  } catch (std::runtime_error const &) {
    // expected
  }

  std::filesystem::remove_all(fake.parent_path());
  std::filesystem::remove_all(dest);
}

TEST_CASE("compute_archive_totals on bare .gz reports one file with bytes") {
  auto const archive{ std::filesystem::path("test_data/archives/hello.txt.gz") };
  envy::extract_totals const totals{ envy::compute_archive_totals(archive) };
  CHECK(totals.files == 1);
  CHECK(totals.bytes > 0);
}

TEST_CASE("compute_archive_totals on corrupt .gz throws") {
  // Mirror extract()'s validation: prescan must not silently report a corrupt
  // bare-compressed file as a valid 1-file archive.
  auto const archive{ std::filesystem::path("test_data/archives/corrupt.gz") };
  try {
    envy::compute_archive_totals(archive);
    FAIL("Expected exception for corrupt .gz");
  } catch (std::runtime_error const &e) { CHECK(std::string{ e.what() }.size() > 0); }
}

TEST_CASE("archive_create_tar_zst with fetch prefix") {
  auto const source{ make_temp_dir() };
  auto const dest{ make_temp_dir() };

  { std::ofstream{ source / "archive.tar.gz" } << "fake archive data"; }

  auto const archive{ make_temp_dir() / "test.tar.zst" };
  envy::archive_create_tar_zst(archive, source, "fetch");

  envy::extract(archive, dest);

  auto const files{ collect_files_recursive(dest) };
  CHECK(files.size() == 1);
  CHECK(files[0] == "fetch/archive.tar.gz");

  std::filesystem::remove_all(source);
  std::filesystem::remove_all(dest);
  std::filesystem::remove_all(archive.parent_path());
}

TEST_CASE("extract_is_safe_archive_path rejects escape vectors") {
  CHECK(envy::extract_is_safe_archive_path("a.txt"));
  CHECK(envy::extract_is_safe_archive_path("a/b/c.txt"));
  CHECK(envy::extract_is_safe_archive_path("a/..b/c"));  // ".." substring, not component
  CHECK(envy::extract_is_safe_archive_path("a/b../c"));
  CHECK(envy::extract_is_safe_archive_path("..a/b"));
  CHECK(envy::extract_is_safe_archive_path("a..b"));

  CHECK_FALSE(envy::extract_is_safe_archive_path(nullptr));
  CHECK_FALSE(envy::extract_is_safe_archive_path(""));
  CHECK_FALSE(envy::extract_is_safe_archive_path("/abs/path"));
  CHECK_FALSE(envy::extract_is_safe_archive_path("\\abs\\path"));
  CHECK_FALSE(envy::extract_is_safe_archive_path(".."));
  CHECK_FALSE(envy::extract_is_safe_archive_path("../x"));
  CHECK_FALSE(envy::extract_is_safe_archive_path("a/../../x"));
  CHECK_FALSE(envy::extract_is_safe_archive_path("a/.."));
  CHECK_FALSE(envy::extract_is_safe_archive_path("a\\..\\x"));
#ifdef _WIN32
  CHECK_FALSE(envy::extract_is_safe_archive_path("C:\\evil"));
  CHECK_FALSE(envy::extract_is_safe_archive_path("c:/evil"));
#endif
}

TEST_CASE("extract_canonical_match_path normalizes separators and decoration") {
  CHECK(envy::extract_canonical_match_path("bin/clang") == "bin/clang");
  CHECK(envy::extract_canonical_match_path("bin\\clang") == "bin/clang");
  CHECK(envy::extract_canonical_match_path("./bin/clang") == "bin/clang");
  CHECK(envy::extract_canonical_match_path("././bin") == "bin");
  CHECK(envy::extract_canonical_match_path("bin/") == "bin");
  CHECK(envy::extract_canonical_match_path("bin///") == "bin");
  CHECK(envy::extract_canonical_match_path(".\\bin\\") == "bin");
  CHECK(envy::extract_canonical_match_path("").empty());
  CHECK(envy::extract_canonical_match_path("./").empty());

  // Repeated separators collapse, so a canonical path never has an empty component.
  CHECK(envy::extract_canonical_match_path("a//b") == "a/b");
  CHECK(envy::extract_canonical_match_path("a///b////c") == "a/b/c");
  CHECK(envy::extract_canonical_match_path("a\\\\b") == "a/b");
  CHECK(envy::extract_canonical_match_path("a/\\b") == "a/b");
  CHECK(envy::extract_canonical_match_path(".//a//") == "a");
}

TEST_CASE("extract_glob_match: literal entries keep exact-or-subtree semantics") {
  CHECK(envy::extract_glob_match("bin/clang-format", "bin/clang-format"));
  CHECK(envy::extract_glob_match("bin", "bin"));
  CHECK(envy::extract_glob_match("bin", "bin/clang"));
  CHECK(envy::extract_glob_match("lib/clang", "lib/clang/19/include/stdatomic.h"));

  CHECK_FALSE(envy::extract_glob_match("bin/clang-format", "bin/clang-format-diff"));
  CHECK_FALSE(envy::extract_glob_match("bin/clang-format", "bin"));
  CHECK_FALSE(envy::extract_glob_match("bin", "binary"));
  CHECK_FALSE(envy::extract_glob_match("lib/clang", "lib/clangd"));
  CHECK_FALSE(envy::extract_glob_match("lib/clang", "usr/lib/clang"));
  CHECK_FALSE(envy::extract_glob_match("a/b/c", "a/b"));
}

TEST_CASE("extract_glob_match: '*' matches within one component only") {
  CHECK(envy::extract_glob_match("bin/clang-*", "bin/clang-format"));
  CHECK(envy::extract_glob_match("bin/clang-*", "bin/clang-tidy"));
  CHECK(envy::extract_glob_match("bin/*", "bin/clangd"));
  CHECK(envy::extract_glob_match("*", "bin"));
  CHECK(envy::extract_glob_match("*/clangd", "bin/clangd"));
  CHECK(envy::extract_glob_match("*.h", "stdatomic.h"));
  CHECK(envy::extract_glob_match("lib*", "libclang.so"));
  CHECK(envy::extract_glob_match("cl*d", "clangd"));
  CHECK(envy::extract_glob_match("*clang*", "libclang.so"));
  CHECK(envy::extract_glob_match("clang*", "clang"));  // '*' matches an empty run
  CHECK(envy::extract_glob_match("**", "a/b/c"));
  CHECK_FALSE(envy::extract_glob_match("*", ""));  // the archive root is unselectable

  CHECK_FALSE(envy::extract_glob_match("bin/clang-*", "bin/clang"));
  CHECK_FALSE(envy::extract_glob_match("bin/*.h", "bin/sub/x.h"));  // no '/' crossing
  CHECK_FALSE(envy::extract_glob_match("a*c", "ab/c"));
  CHECK_FALSE(envy::extract_glob_match("*.h", "x.hpp"));
  CHECK_FALSE(envy::extract_glob_match("*.h", "h"));
  CHECK_FALSE(envy::extract_glob_match("bin/*", "bin"));  // '*' needs a component
}

TEST_CASE("extract_glob_match: '?' matches exactly one character") {
  CHECK(envy::extract_glob_match("file?.txt", "file1.txt"));
  CHECK(envy::extract_glob_match("subdir?", "subdir2"));
  CHECK(envy::extract_glob_match("??", "ab"));
  CHECK(envy::extract_glob_match("a?c/d", "abc/d"));

  CHECK_FALSE(envy::extract_glob_match("file?.txt", "file.txt"));
  CHECK_FALSE(envy::extract_glob_match("file?.txt", "file12.txt"));
  CHECK_FALSE(envy::extract_glob_match("a?c", "a/c"));  // '?' never matches '/'
  CHECK_FALSE(envy::extract_glob_match("??", "a"));
}

TEST_CASE("extract_glob_match: '[...]' character classes") {
  CHECK(envy::extract_glob_match("file[123].txt", "file2.txt"));
  CHECK(envy::extract_glob_match("file[0-9].txt", "file7.txt"));
  CHECK(envy::extract_glob_match("[a-z]*", "clangd"));
  CHECK(envy::extract_glob_match("[a-cx-z]", "y"));
  CHECK(envy::extract_glob_match("[!0-9]*", "clangd"));
  CHECK(envy::extract_glob_match("[^0-9]*", "clangd"));
  CHECK(envy::extract_glob_match("[a-]", "-"));
  CHECK(envy::extract_glob_match("[-a]", "-"));
  CHECK(envy::extract_glob_match("[]]", "]"));
  CHECK(envy::extract_glob_match("[*]", "*"));  // metacharacters go literal in a class
  CHECK(envy::extract_glob_match("[?]", "?"));
  CHECK(envy::extract_glob_match("v[0-9].[0-9]", "v1.2"));
  CHECK(envy::extract_glob_match("*[0-9]", "file1"));  // class after a backtracking '*'

  CHECK_FALSE(envy::extract_glob_match("file[123].txt", "file4.txt"));
  CHECK_FALSE(envy::extract_glob_match("file[0-9].txt", "filex.txt"));
  CHECK_FALSE(envy::extract_glob_match("[!0-9]*", "1clangd"));
  CHECK_FALSE(envy::extract_glob_match("[^0-9]*", "1clangd"));
  CHECK_FALSE(envy::extract_glob_match("[*]", "x"));
  CHECK_FALSE(envy::extract_glob_match("a[/]b", "a/b"));  // classes never span components
  CHECK_FALSE(envy::extract_glob_match("[a-c]", "d"));
}

TEST_CASE("extract_glob_match: '**' spans components") {
  CHECK(envy::extract_glob_match("**/file4.txt", "file4.txt"));
  CHECK(envy::extract_glob_match("**/file4.txt", "root/file4.txt"));
  CHECK(envy::extract_glob_match("**/file4.txt", "root/a/b/c/file4.txt"));
  CHECK(envy::extract_glob_match("root/**", "root"));
  CHECK(envy::extract_glob_match("root/**", "root/a/b"));
  CHECK(envy::extract_glob_match("a/**/b", "a/b"));
  CHECK(envy::extract_glob_match("a/**/b", "a/x/b"));
  CHECK(envy::extract_glob_match("a/**/b", "a/x/y/b"));
  CHECK(envy::extract_glob_match("**", "anything/at/all"));
  CHECK(envy::extract_glob_match("lib/**/include/*.h", "lib/clang/20/include/atomic.h"));
  CHECK(envy::extract_glob_match("**/x/**/y", "x/y"));
  CHECK(envy::extract_glob_match("**/x/**/y", "a/b/x/c/d/y"));
  CHECK(envy::extract_glob_match("**/b/**/c", "b/x/b/y/c"));
  CHECK(envy::extract_glob_match("**/*.h", "a/b/c.h"));

  CHECK_FALSE(envy::extract_glob_match("a/**/b", "a/x/y/c"));
  CHECK_FALSE(envy::extract_glob_match("a/**", "ab/c"));
  CHECK_FALSE(envy::extract_glob_match("**/file4.txt", "root/file4.txt.bak"));
  CHECK_FALSE(envy::extract_glob_match("**/x/**/y", "x/z"));
  CHECK_FALSE(envy::extract_glob_match("lib/**/include/*.h", "lib/clang/include/x.hpp"));
}

TEST_CASE("extract_glob_match: pathological patterns terminate without matching") {
  // One saved star per level keeps this linear; a recursive matcher would blow up here.
  CHECK_FALSE(
      envy::extract_glob_match("a*a*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
  CHECK(envy::extract_glob_match("a*a*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab"));
  CHECK_FALSE(envy::extract_glob_match("**/**/**/x", "a/b/c/d/e/f/g/h/i/j"));
  CHECK(envy::extract_glob_match("**/**/**/x", "a/b/c/d/e/f/g/h/i/x"));
  CHECK_FALSE(envy::extract_glob_match("*?*?*?*?*z", "aaaaaaaaaaaaaaaaaaaa"));
}

TEST_CASE("extract_glob_match: matching is case sensitive on every platform") {
  CHECK_FALSE(envy::extract_glob_match("bin/Clang", "bin/clang"));
  CHECK_FALSE(envy::extract_glob_match("*.H", "x.h"));
  CHECK_FALSE(envy::extract_glob_match("[a-z]", "A"));
  CHECK(envy::extract_glob_match("[A-Za-z]", "A"));
}

TEST_CASE("extract_normalize_selectors canonicalizes and rejects unusable entries") {
  auto const ok{ envy::extract_normalize_selectors({ "./bin/", "lib\\clang" }, "ctx") };
  REQUIRE(ok.size() == 2);
  CHECK(ok[0] == "bin");
  CHECK(ok[1] == "lib/clang");

  for (auto const &bad : { "", ".", "/abs", "../escape", "bin/../../escape" }) {
    try {
      envy::extract_normalize_selectors({ bad }, "ctx");
      FAIL("Expected rejection of 'only' entry: " << bad);
    } catch (std::runtime_error const &e) {
      CHECK(std::string{ e.what() }.find("ctx: 'only' entry") != std::string::npos);
    }
  }
}

TEST_CASE("extract_normalize_selectors accepts well-formed glob patterns") {
  auto const ok{ envy::extract_normalize_selectors(
      { "bin/clang-*", "lib/**/include/*.h", "[a-z]?[!0-9]", "**", "a/**/b", "[]]" },
      "ctx") };
  REQUIRE(ok.size() == 6);
  CHECK(ok[0] == "bin/clang-*");
  CHECK(ok[1] == "lib/**/include/*.h");
  CHECK(ok[2] == "[a-z]?[!0-9]");
  CHECK(ok[3] == "**");
  CHECK(ok[4] == "a/**/b");
  CHECK(ok[5] == "[]]");
}

TEST_CASE("extract_normalize_selectors rejects malformed glob patterns") {
  auto const expect_reject{ [](char const *bad, char const *needle) {
    try {
      envy::extract_normalize_selectors({ bad }, "ctx");
      FAIL("Expected rejection of glob pattern: " << bad);
    } catch (std::runtime_error const &e) {
      std::string const msg{ e.what() };
      CHECK(msg.find("ctx: 'only' entry") != std::string::npos);
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

TEST_CASE("extract_selectors_match flags every matching pattern") {
  std::vector<std::string> const selectors{ "bin/*", "**/*.h", "share" };
  std::vector<bool> matched;

  CHECK(envy::extract_selectors_match(selectors, "bin/clang", matched));
  CHECK(envy::extract_selectors_match(selectors, "lib/clang/20/x.h", matched));
  CHECK_FALSE(envy::extract_selectors_match(selectors, "docs/readme.md", matched));

  auto const unmatched{ envy::extract_unmatched_selectors(selectors, matched) };
  REQUIRE(unmatched.size() == 1);
  CHECK(unmatched[0] == "share");

  // One entry satisfying two patterns marks both.
  std::vector<std::string> const overlap{ "bin/*", "bin/clang" };
  std::vector<bool> overlap_matched;
  CHECK(envy::extract_selectors_match(overlap, "bin/clang", overlap_matched));
  CHECK(envy::extract_unmatched_selectors(overlap, overlap_matched).empty());
}

TEST_CASE("extract_selectors_match matches files exactly and directories by subtree") {
  std::vector<std::string> const selectors{ "bin/clang-format", "lib/clang" };
  std::vector<bool> matched;

  CHECK(envy::extract_selectors_match(selectors, "bin/clang-format", matched));
  CHECK(envy::extract_selectors_match(selectors, "lib/clang", matched));
  CHECK(envy::extract_selectors_match(selectors,
                                      "lib/clang/19/include/stdatomic.h",
                                      matched));

  // Prefix-of-a-name is not a subtree; neither is a parent of a selected entry.
  CHECK_FALSE(envy::extract_selectors_match(selectors, "bin/clang-format-diff", matched));
  CHECK_FALSE(envy::extract_selectors_match(selectors, "bin", matched));
  CHECK_FALSE(envy::extract_selectors_match(selectors, "lib/clangd", matched));
  CHECK_FALSE(envy::extract_selectors_match(selectors, "share/man", matched));

  CHECK(envy::extract_unmatched_selectors(selectors, matched).empty());
}

TEST_CASE("extract_selectors_match flags every matching entry, not just the first") {
  // Overlapping entries must all count as matched, else a redundant-but-valid selector
  // list would look unsatisfied.
  std::vector<std::string> const selectors{ "bin", "bin/clang", "share" };
  std::vector<bool> matched;

  CHECK(envy::extract_selectors_match(selectors, "bin/clang", matched));

  auto const unmatched{ envy::extract_unmatched_selectors(selectors, matched) };
  REQUIRE(unmatched.size() == 1);
  CHECK(unmatched[0] == "share");
}

TEST_CASE("extract with only takes one named file") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  envy::extract_options const opts{ .selectors = { "root/subdir1/nested/file4.txt" } };
  CHECK(envy::extract(archive, dest, opts) == 1);

  auto const files{ collect_files_recursive(dest) };
  REQUIRE(files.size() == 1);
  CHECK(files[0] == "root/subdir1/nested/file4.txt");

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract with only takes a whole directory subtree") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  envy::extract_options const opts{ .selectors = { "root/subdir1" } };
  CHECK(envy::extract(archive, dest, opts) == 2);

  auto const files{ collect_files_recursive(dest) };
  REQUIRE(files.size() == 2);
  CHECK(files[0] == "root/subdir1/file3.txt");
  CHECK(files[1] == "root/subdir1/nested/file4.txt");
  CHECK_FALSE(std::filesystem::exists(dest / "root" / "subdir2"));

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract only entries are matched after strip_components") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  envy::extract_options const opts{ .strip_components = 1,
                                    .selectors = { "subdir2", "file1.txt" } };
  CHECK(envy::extract(archive, dest, opts) == 2);

  auto const files{ collect_files_recursive(dest) };
  REQUIRE(files.size() == 2);
  CHECK(files[0] == "file1.txt");
  CHECK(files[1] == "subdir2/file5.txt");

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract only entry with trailing slash and ./ prefix still matches") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  envy::extract_options const opts{ .selectors = { "./root/subdir2/" } };
  CHECK(envy::extract(archive, dest, opts) == 1);

  auto const files{ collect_files_recursive(dest) };
  REQUIRE(files.size() == 1);
  CHECK(files[0] == "root/subdir2/file5.txt");

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract with glob only selects matching entries") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };
  std::vector<std::string> only;
  std::vector<std::string> expected;
  int strip{ 0 };

  SUBCASE("'*' selects one component") {
    only = { "root/*.txt" };
    expected = { "root/file1.txt", "root/file2.txt" };
  }

  SUBCASE("'*' as an interior component") {
    only = { "root/*/file3.txt" };
    expected = { "root/subdir1/file3.txt" };
  }

  SUBCASE("'**' reaches any depth") {
    only = { "root/**/file4.txt" };
    expected = { "root/subdir1/nested/file4.txt" };
  }

  SUBCASE("'?' selects sibling directories, each with its subtree") {
    only = { "root/subdir?" };
    expected = { "root/subdir1/file3.txt",
                 "root/subdir1/nested/file4.txt",
                 "root/subdir2/file5.txt" };
  }

  SUBCASE("character class selects named files") {
    only = { "root/file[12].txt" };
    expected = { "root/file1.txt", "root/file2.txt" };
  }

  SUBCASE("'**' with a trailing pattern takes everything") {
    only = { "**/*.txt" };
    expected = { "root/file1.txt",
                 "root/file2.txt",
                 "root/subdir1/file3.txt",
                 "root/subdir1/nested/file4.txt",
                 "root/subdir2/file5.txt" };
  }

  SUBCASE("patterns match post-strip paths") {
    strip = 1;
    only = { "subdir1/**" };
    expected = { "subdir1/file3.txt", "subdir1/nested/file4.txt" };
  }

  SUBCASE("two patterns union") {
    only = { "root/file1.txt", "root/sub*2/*" };
    expected = { "root/file1.txt", "root/subdir2/file5.txt" };
  }

  envy::extract_options const opts{ .strip_components = strip, .selectors = only };
  CHECK(envy::extract(archive, dest, opts) == expected.size());
  CHECK(collect_files_recursive(dest) == expected);

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract throws when a glob only entry matches nothing") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  envy::extract_options const opts{ .selectors = { "root/*.txt", "root/**/*.md" } };

  try {
    envy::extract(archive, dest, opts);
    FAIL("Expected exception for unmatched glob pattern");
  } catch (std::runtime_error const &e) {
    std::string const msg{ e.what() };
    CHECK(msg.find("\"root/**/*.md\"") != std::string::npos);
    CHECK(msg.find("\"root/*.txt\"") == std::string::npos);
  }

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract rejects a malformed glob before touching the archive") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  for (auto const *bad : { "root/[abc", "root/a**b" }) {
    try {
      envy::extract(archive, dest, { .selectors = { bad } });
      FAIL("Expected exception for malformed glob: " << bad);
    } catch (std::runtime_error const &e) {
      CHECK(std::string{ e.what() }.find("extract: 'only' entry") != std::string::npos);
    }
  }

  CHECK(collect_files_recursive(dest).empty());
  std::filesystem::remove_all(dest);
}

TEST_CASE("extract_all_archives with glob only spans archive and loose files") {
  auto const fetch_dir{ make_temp_dir() };
  auto const dest{ make_temp_dir() };
  std::filesystem::copy_file(std::filesystem::path("test_data/archives/test.tar.gz"),
                             fetch_dir / "test.tar.gz");
  { std::ofstream{ fetch_dir / "notes.md", std::ios::binary } << "notes"; }
  { std::ofstream{ fetch_dir / "skipped.bin", std::ios::binary } << "unwanted"; }

  envy::extract_all_archives(
      fetch_dir,
      dest,
      { .strip_components = 1, .selectors = { "**/file4.txt", "*.md" } },
      "test.pkg@v1",
      envy::tui::kInvalidSection);

  auto const files{ collect_files_recursive(dest) };
  REQUIRE(files.size() == 2);
  CHECK(files[0] == "notes.md");
  CHECK(files[1] == "subdir1/nested/file4.txt");

  std::filesystem::remove_all(fetch_dir);
  std::filesystem::remove_all(dest);
}

TEST_CASE("compute_archive_totals with a glob counts only matching entries") {
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  CHECK(envy::compute_archive_totals(archive, { .selectors = { "root/*.txt" } }).files ==
        2);
  CHECK(envy::compute_archive_totals(archive, { .selectors = { "**/*.txt" } }).files == 5);
  CHECK(envy::compute_archive_totals(archive, { .selectors = { "root/subdir?" } }).files ==
        3);

  auto const missing{ envy::compute_archive_totals(archive,
                                                   { .selectors = { "root/*.md" } }) };
  CHECK(missing.files == 0);
  REQUIRE(missing.unmatched_selectors.size() == 1);
  CHECK(missing.unmatched_selectors[0] == "root/*.md");
}

TEST_CASE("extract throws when an only entry matches nothing") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  envy::extract_options const opts{ .selectors = { "root/file1.txt", "root/nope" } };

  try {
    envy::extract(archive, dest, opts);
    FAIL("Expected exception for unmatched selector");
  } catch (std::runtime_error const &e) {
    std::string const msg{ e.what() };
    CHECK(msg.find("test.tar.gz") != std::string::npos);
    CHECK(msg.find("\"root/nope\"") != std::string::npos);
    CHECK(msg.find("\"root/file1.txt\"") == std::string::npos);
  }

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract rejects an unusable only entry") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  envy::extract_options const opts{ .selectors = { "../escape" } };

  try {
    envy::extract(archive, dest, opts);
    FAIL("Expected exception for traversal in selector");
  } catch (std::runtime_error const &e) {
    CHECK(std::string{ e.what() }.find("extract: 'only' entry") != std::string::npos);
  }

  CHECK(collect_files_recursive(dest).empty());
  std::filesystem::remove_all(dest);
}

TEST_CASE("compute_archive_totals with only counts only selected entries") {
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  envy::extract_totals const all{ envy::compute_archive_totals(archive) };
  envy::extract_totals const subtree{
    envy::compute_archive_totals(archive, { .selectors = { "root/subdir1" } })
  };

  CHECK(all.files == 5);
  CHECK(subtree.files == 2);
  CHECK(subtree.bytes < all.bytes);
  CHECK(subtree.unmatched_selectors.empty());

  envy::extract_totals const missing{ envy::compute_archive_totals(
      archive,
      { .selectors = { "root/subdir1", "root/nope" } }) };
  REQUIRE(missing.unmatched_selectors.size() == 1);
  CHECK(missing.unmatched_selectors[0] == "root/nope");
}

TEST_CASE("compute_extract_totals only spans archives and loose files") {
  auto const fetch_dir{ make_temp_dir() };
  std::filesystem::copy_file(std::filesystem::path("test_data/archives/test.tar.gz"),
                             fetch_dir / "test.tar.gz");
  { std::ofstream{ fetch_dir / "plain.txt", std::ios::binary } << "hello world"; }

  // "plain.txt" comes from the loose file, "root/subdir2" from inside the archive:
  // neither alone satisfies the list, so the union is what gets validated.
  envy::extract_totals const totals{ envy::compute_extract_totals(
      fetch_dir,
      { .selectors = { "plain.txt", "root/subdir2" } }) };

  CHECK(totals.files == 2);
  CHECK(totals.unmatched_selectors.empty());

  envy::extract_totals const missing{
    envy::compute_extract_totals(fetch_dir, { .selectors = { "plain.txt", "nope" } })
  };
  REQUIRE(missing.unmatched_selectors.size() == 1);
  CHECK(missing.unmatched_selectors[0] == "nope");

  std::filesystem::remove_all(fetch_dir);
}

TEST_CASE("extract_all_archives with only copies loose files and archive subsets") {
  auto const fetch_dir{ make_temp_dir() };
  auto const dest{ make_temp_dir() };
  std::filesystem::copy_file(std::filesystem::path("test_data/archives/test.tar.gz"),
                             fetch_dir / "test.tar.gz");
  { std::ofstream{ fetch_dir / "plain.txt", std::ios::binary } << "hello world"; }
  { std::ofstream{ fetch_dir / "skipped.txt", std::ios::binary } << "unwanted"; }

  envy::extract_all_archives(
      fetch_dir,
      dest,
      { .strip_components = 1, .selectors = { "subdir2", "plain.txt" } },
      "test.pkg@v1",
      envy::tui::kInvalidSection);

  auto const files{ collect_files_recursive(dest) };
  REQUIRE(files.size() == 2);
  CHECK(files[0] == "plain.txt");
  CHECK(files[1] == "subdir2/file5.txt");

  std::filesystem::remove_all(fetch_dir);
  std::filesystem::remove_all(dest);
}

TEST_CASE("extract_all_archives throws when nothing in the fetch dir matches a selector") {
  auto const fetch_dir{ make_temp_dir() };
  auto const dest{ make_temp_dir() };
  std::filesystem::copy_file(std::filesystem::path("test_data/archives/test.tar.gz"),
                             fetch_dir / "test.tar.gz");

  try {
    envy::extract_all_archives(fetch_dir,
                               dest,
                               { .selectors = { "root/subdir2", "root/nope" } },
                               "test.pkg@v1",
                               envy::tui::kInvalidSection);
    FAIL("Expected exception for unmatched selector");
  } catch (std::runtime_error const &e) {
    CHECK(std::string{ e.what() }.find("\"root/nope\"") != std::string::npos);
  }

  std::filesystem::remove_all(fetch_dir);
  std::filesystem::remove_all(dest);
}
