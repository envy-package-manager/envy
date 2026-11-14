#include "extract.h"

#include "glob.h"

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

TEST_CASE(
    "extract_probe_totals counts plain tar and zip headers, strip and 'only' applied") {
  // source/root: file1.txt, file2.txt, subdir1/file3.txt (18B each),
  // subdir1/nested/file4.txt (20B), subdir2/file5.txt (19B).
  struct row {
    envy::extract_options opts;
    envy::extract_totals want;
  };
  std::vector<row> const rows{
    { {}, { .files = 5, .bytes = 93 } },
    { { .strip_components = 1 }, { .files = 5, .bytes = 93 } },
    { { .strip_components = 3 }, { .files = 1, .bytes = 20 } },
    { { .strip_components = 4 }, { .files = 0, .bytes = 0 } },
    { { .selectors = { "root/*.txt" } }, { .files = 2, .bytes = 36 } },
    { { .selectors = { "**/*.txt" } }, { .files = 5, .bytes = 93 } },
    { { .selectors = { "root/subdir?" } }, { .files = 3, .bytes = 57 } },
    { { .selectors = { "root/**", "!root/subdir1/**" } }, { .files = 3, .bytes = 55 } },
    { { .strip_components = 1, .selectors = { "subdir1" } }, { .files = 2, .bytes = 38 } },
    { { .selectors = { "root/nope" } }, { .files = 0, .bytes = 0 } },
  };
  for (auto const *name : { "test.tar", "test.zip" }) {
    for (std::size_t i{ 0 }; i < rows.size(); ++i) {
      CHECK_MESSAGE(
          envy::extract_probe_totals(std::filesystem::path("test_data/archives") / name,
                                     rows[i].opts) == rows[i].want,
          name,
          " row ",
          i);
    }
  }
}

TEST_CASE("extract_probe_totals will not decompress an archive to count it") {
  for (auto const *name : { "test.tar.gz",
                            "test.tar.bz2",
                            "test.tar.xz",
                            "test.tar.zst",
                            "hello.txt.gz",
                            "corrupt.gz" }) {
    CHECK_MESSAGE(
        !envy::extract_probe_totals(std::filesystem::path("test_data/archives") / name),
        name);
  }
}

TEST_CASE("extract_probe_totals reads envy's trailer, for the whole archive only") {
  // test.tar compressed, then trailed as archive_create_tar_zst trails its output.
  auto const stated{ std::filesystem::path("test_data/archives/stated.tar.zst") };
  CHECK(envy::extract_probe_totals(stated) ==
        envy::extract_totals{ .files = 5, .bytes = 93 });

  // A selection is a subset the trailer cannot speak for, and the payload is compressed.
  CHECK_FALSE(envy::extract_probe_totals(stated, { .selectors = { "root/subdir1" } }));
  CHECK_FALSE(envy::extract_probe_totals(stated, { .strip_components = 1 }));
}

TEST_CASE("extract trailer: a zstd skippable frame stating files and bytes") {
  envy::extract_trailer const want{
    0x55, 0x2A, 0x4D, 0x18,                    // skippable-frame magic, little-endian
    24,   0,    0,    0,                       // payload size
    5,    0,    0,    0,    0,   0,   0,   0,  // files
    93,   0,    0,    0,    0,   0,   0,   0,  // bytes
    'e',  'n',  'v',  'y',  't', 'o', 't', '1'
  };
  CHECK(envy::extract_trailer_encode({ .files = 5, .bytes = 93 }) == want);
  CHECK(envy::extract_trailer_decode(want) ==
        envy::extract_totals{ .files = 5, .bytes = 93 });

  envy::extract_totals const wide{ .files = 0x0102030405060708, .bytes = UINT64_MAX };
  CHECK(envy::extract_trailer_decode(envy::extract_trailer_encode(wide)) == wide);
}

TEST_CASE("extract trailer: damage to its frame or tag reads as no trailer") {
  auto const good{ envy::extract_trailer_encode({ .files = 5, .bytes = 93 }) };
  for (std::size_t i{ 0 }; i < good.size(); ++i) {
    auto bad{ good };
    bad[i] ^= 0x01;
    bool const counted{ i >= 8 && i < 24 };  // files and bytes: any value is a value
    CHECK_MESSAGE(envy::extract_trailer_decode(bad).has_value() == counted, "byte ", i);
  }
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

TEST_CASE("extract_all_archives unpacks a file 'archives' names, whatever its extension") {
  auto const fetch_dir{ make_temp_dir() };
  auto const dest{ make_temp_dir() };
  std::filesystem::copy_file(std::filesystem::path("test_data/archives/test.zip"),
                             fetch_dir / "sdk.pack");

  envy::extract_all_archives(
      fetch_dir,
      dest,
      { .strip_components = 1, .selectors = { "subdir2" }, .archives = { "*.pack" } },
      "test.pkg@v1",
      envy::tui::kInvalidSection);

  CHECK(collect_files_recursive(dest) == std::vector<std::string>{ "subdir2/file5.txt" });

  std::filesystem::remove_all(fetch_dir);
  std::filesystem::remove_all(dest);
}

TEST_CASE("extract_all_archives copies a file whole when a '!' archives entry names it") {
  auto const fetch_dir{ make_temp_dir() };
  auto const dest{ make_temp_dir() };
  std::filesystem::copy_file(std::filesystem::path("test_data/archives/test.tar.gz"),
                             fetch_dir / "test.tar.gz");
  std::filesystem::copy_file(std::filesystem::path("test_data/archives/test.zip"),
                             fetch_dir / "test.zip");

  envy::extract_all_archives(fetch_dir,
                             dest,
                             { .strip_components = 1, .archives = { "!*.tar.gz" } },
                             "test.pkg@v1",
                             envy::tui::kInvalidSection);

  auto const files{ collect_files_recursive(dest) };
  CHECK(std::ranges::find(files, "test.tar.gz") != files.end());
  CHECK(std::ranges::find(files, "subdir2/file5.txt") != files.end());  // zip unpacked
  CHECK(files.size() == 6);

  std::filesystem::remove_all(fetch_dir);
  std::filesystem::remove_all(dest);
}

TEST_CASE("extract_all_archives rejects an 'archives' entry that names no file") {
  auto const fetch_dir{ make_temp_dir() };
  auto const dest{ make_temp_dir() };
  std::filesystem::copy_file(std::filesystem::path("test_data/archives/test.tar.gz"),
                             fetch_dir / "test.tar.gz");

  try {
    envy::extract_all_archives(fetch_dir,
                               dest,
                               { .archives = { "*.tar.gz", "*.pak" } },
                               "test.pkg@v1",
                               envy::tui::kInvalidSection);
    FAIL("Expected exception for unmatched 'archives' entry");
  } catch (std::runtime_error const &e) {
    std::string const msg{ e.what() };
    CHECK(msg.find("'archives'") != std::string::npos);
    CHECK(msg.find("\"*.pak\"") != std::string::npos);
    CHECK(msg.find("\"*.tar.gz\"") == std::string::npos);
  }
  CHECK(collect_files_recursive(dest).empty());

  std::filesystem::remove_all(fetch_dir);
  std::filesystem::remove_all(dest);
}

TEST_CASE("extract_all_archives names the misnamed archive an unmatched 'only' missed") {
  auto const fetch_dir{ make_temp_dir() };
  auto const dest{ make_temp_dir() };
  std::filesystem::copy_file(std::filesystem::path("test_data/archives/test.zip"),
                             fetch_dir / "sdk.pack");
  { std::ofstream{ fetch_dir / "notes.txt", std::ios::binary } << "notes"; }

  try {
    envy::extract_all_archives(fetch_dir,
                               dest,
                               { .selectors = { "root/file1.txt" } },
                               "test.pkg@v1",
                               envy::tui::kInvalidSection);
    FAIL("Expected exception for unmatched selector");
  } catch (std::runtime_error const &e) {
    std::string const msg{ e.what() };
    CHECK(msg.find("\"root/file1.txt\"") != std::string::npos);
    CHECK(msg.find("\"sdk.pack\"") != std::string::npos);
    CHECK(msg.find("'archives'") != std::string::npos);
    CHECK(msg.find("notes.txt") == std::string::npos);  // not an archive, so not named
  }

  std::filesystem::remove_all(fetch_dir);
  std::filesystem::remove_all(dest);
}

TEST_CASE("extract 'only' excludes with a leading '!'") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  envy::extract_options const opts{ .selectors = { "root/**", "!root/subdir1/**" } };
  CHECK(envy::extract(archive, dest, opts) == 3);

  auto const files{ collect_files_recursive(dest) };
  REQUIRE(files.size() == 3);
  CHECK(files[0] == "root/file1.txt");
  CHECK(files[1] == "root/file2.txt");
  CHECK(files[2] == "root/subdir2/file5.txt");

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract 'only' with excludes alone takes everything else") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  envy::extract_options const opts{ .selectors = { "!root/subdir1" } };
  envy::extract(archive, dest, opts);

  auto const files{ collect_files_recursive(dest) };
  REQUIRE(files.size() == 3);
  CHECK(std::ranges::none_of(files, [](std::string const &f) {
    return f.starts_with("root/subdir1/");
  }));

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract 'only' exclusion beats inclusion") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  envy::extract_options const opts{ .selectors = { "root/subdir1/nested/file4.txt",
                                                   "!root/subdir1/**" } };
  envy::extract(archive, dest, opts);
  CHECK(collect_files_recursive(dest).empty());

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract reports an include that named nothing, but tolerates such an exclude") {
  auto const dest{ make_temp_dir() };
  auto const archive{ std::filesystem::path("test_data/archives/test.tar.gz") };

  // An unmatched include is a typo an author wants told about; an unmatched exclude is
  // just a set that happened not to contain the thing.
  CHECK_THROWS_AS(
      envy::extract(archive, dest, { .selectors = { "root/file1.txt", "root/nope" } }),
      std::runtime_error);
  CHECK_NOTHROW(
      envy::extract(archive, dest, { .selectors = { "root/file1.txt", "!root/nope" } }));

  std::filesystem::remove_all(dest);
}

TEST_CASE("extract selectors and a spec's VENDOR list are one language") {
  // The guarantee behind sharing glob_filter: the same written patterns choose the same
  // paths whether they arrive as `only` on an archive or as VENDOR on a payload.
  std::vector<std::string> const written{ "root/**", "!root/subdir1/**" };
  auto const filter{ envy::glob_parse_filter(written, "test", "entry") };

  auto const dest{ make_temp_dir() };
  envy::extract(std::filesystem::path("test_data/archives/test.tar.gz"),
                dest,
                { .selectors = written });

  for (auto const &extracted : collect_files_recursive(dest)) {
    CHECK_MESSAGE(filter.selects(extracted), "extracted but not selected: ", extracted);
  }
  for (auto const *skipped :
       { "root/subdir1/file3.txt", "root/subdir1/nested/file4.txt" }) {
    CHECK_FALSE(filter.selects(skipped));
  }

  std::filesystem::remove_all(dest);
}
