#include "tree_hash.h"

#include "blake3_util.h"
#include "file_read.h"
#include "util.h"

#include "doctest.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path const kBasic{ "test_data/tree_hash/basic" };

// 3 and 17 are unround on purpose: a drain bug that needs workers to outnumber
// directories, or not divide evenly into them, hides behind 1/2/4/8.
constexpr unsigned kThreadCounts[]{ 1, 2, 3, 4, 8, 17 };

std::string hex(envy::blake3_t const &d) {
  return envy::util_bytes_to_hex(d.data(), d.size());
}

std::string digest_of(fs::path const &root,
                      envy::tree_filter const &filter = {},
                      unsigned threads = 1) {
  return hex(envy::tree_hash(root, filter, threads).digest);
}

// A dumb reference: recursive std::filesystem walk, whole files in memory, std::map for
// ordering. Shares no code with the threaded walker, so agreement is evidence.
std::string oracle_digest(fs::path const &root, envy::tree_filter const &filter = {}) {
  std::map<std::string, std::vector<unsigned char>> folded;

  auto const relative_of{ [&root](fs::path const &p) {
    return p.lexically_relative(root).generic_string();
  } };

  for (auto it{ fs::recursive_directory_iterator(
           root, fs::directory_options::skip_permission_denied) };
       it != fs::recursive_directory_iterator{};
       ++it) {
    auto const rel{ relative_of(it->path()) };
    if (!filter.selects(rel)) { continue; }

    std::vector<unsigned char> payload;
    auto const push{ [&payload](void const *p, size_t n) {
      auto const *b{ static_cast<unsigned char const *>(p) };
      payload.insert(payload.end(), b, b + n);
    } };

    auto const status{ fs::symlink_status(it->path()) };
    if (fs::is_symlink(status)) {
      payload.push_back('l');
      payload.push_back('\0');
      auto const target{ fs::read_symlink(it->path()).string() };
      push(target.data(), target.size());
      payload.push_back('\0');
    } else if (fs::is_directory(status)) {
      payload.push_back('d');
      payload.push_back('\0');
    } else {
      std::ifstream in{ it->path(), std::ios::binary };
      std::vector<unsigned char> const bytes{ std::istreambuf_iterator<char>{ in },
                                              std::istreambuf_iterator<char>{} };
      auto const d{ envy::blake3_hash(bytes.data(), bytes.size()) };
      payload.push_back('f');
      payload.push_back(
          (fs::status(it->path()).permissions() & fs::perms::owner_exec) != fs::perms::none
              ? '\1'
              : '\0');
      push(d.data(), d.size());
    }
    folded.emplace(rel, std::move(payload));
  }

  std::vector<unsigned char> stream;
  for (auto const &[rel, payload] : folded) {
    stream.insert(stream.end(), rel.begin(), rel.end());
    stream.push_back('\0');
    stream.push_back(payload[0]);                       // kind tag
    stream.push_back(payload.size() > 1 ? payload[1] : '\0');  // exec bit
    stream.insert(stream.end(), payload.begin() + 2, payload.end());
  }
  return hex(envy::blake3_hash(stream.data(), stream.size()));
}

}  // namespace

TEST_CASE("tree_hash agrees with a std::filesystem oracle") {
  // The oracle uses one-shot blake3_hash where tree_hash streams chunks, so agreement
  // pins that both digests are the same hasher.
  CHECK(digest_of(kBasic) == oracle_digest(kBasic));
}

TEST_CASE("a file's digest inside the tree is its own blake3_hash") {
  // Stated directly, without the oracle's fold: what tree_hash records for one file is
  // exactly what hashing that file's bytes on their own produces.
  envy::tree_filter const one{ .include = { "bin/data.bin" } };
  auto const bytes{ envy::util_load_file(kBasic / "bin" / "data.bin") };

  envy::blake3_stream expected;
  std::string const rel{ "bin/data.bin" };
  expected.update(rel.data(), rel.size());
  unsigned char const head[]{ '\0', 'f', '\0' };
  expected.update(head, sizeof(head));
  auto const content{ envy::blake3_hash(bytes.data(), bytes.size()) };
  expected.update(content.data(), content.size());

  CHECK(digest_of(kBasic, one) == hex(expected.finalize()));
}

TEST_CASE("tree_hash is invariant across thread counts") {
  auto const want{ digest_of(kBasic, {}, 1) };
  for (unsigned const n : kThreadCounts) { CHECK(digest_of(kBasic, {}, n) == want); }
}

TEST_CASE("tree_hash counts every file and byte exactly once") {
  auto const [digest, files, bytes]{ envy::tree_hash(kBasic) };

  std::uint64_t want_files{ 0 }, want_bytes{ 0 };
  for (auto const &e : fs::recursive_directory_iterator(kBasic)) {
    if (!e.is_regular_file()) { continue; }
    ++want_files;
    want_bytes += e.file_size();
  }
  CHECK(files == want_files);
  CHECK(bytes == want_bytes);

  for (unsigned const n : kThreadCounts) {
    auto const r{ envy::tree_hash(kBasic, {}, n) };
    CHECK(r.files == want_files);
    CHECK(r.bytes == want_bytes);
  }
}

TEST_CASE("tree_hash known answer pins the fold format") {
  // Locks the on-disk digest format: a change here invalidates every stamped vendor
  // hash in every cache, so it must be a deliberate edit, never a silent side effect.
  CHECK(digest_of(kBasic) ==
        "efa9968ed94330f4717f5dd39c8bba681f580f76ecd9046548ed6a632c7a8f3f");
}

TEST_CASE("tree_list reports every selected entry, sorted, with no duplicates") {
  auto const entries{ envy::tree_list(kBasic) };

  std::vector<std::string> paths;
  for (auto const &e : entries) { paths.push_back(e.relpath); }
  CHECK(std::ranges::is_sorted(paths));
  CHECK(std::ranges::adjacent_find(paths) == paths.end());

  CHECK(std::ranges::count(paths, "include") == 1);
  CHECK(std::ranges::count(paths, "include/detail/impl.h") == 1);
  CHECK(std::ranges::count(paths, "docs/guide md.txt") == 1);

  for (unsigned const n : kThreadCounts) {
    auto const other{ envy::tree_list(kBasic, {}, n) };
    REQUIRE(other.size() == entries.size());
    for (size_t i{ 0 }; i < other.size(); ++i) {
      CHECK(other[i].relpath == entries[i].relpath);
    }
  }
}

TEST_CASE("tree_list carries kind and size per entry") {
  std::map<std::string, envy::tree_entry> by_path;
  for (auto &e : envy::tree_list(kBasic)) { by_path.emplace(e.relpath, e); }

  REQUIRE(by_path.count("include"));
  CHECK(by_path.at("include").kind == envy::tree_entry_kind::DIRECTORY);

  REQUIRE(by_path.count("empty"));
  CHECK(by_path.at("empty").kind == envy::tree_entry_kind::FILE);
  CHECK(by_path.at("empty").size == 0);

  REQUIRE(by_path.count("bin/data.bin"));
  CHECK(by_path.at("bin/data.bin").size == 1024);
}

TEST_CASE("tree_filter include list restricts the digest to what it names") {
  envy::tree_filter const headers{ .include = { "include/**" } };
  CHECK(digest_of(kBasic, headers) == oracle_digest(kBasic, headers));
  CHECK(digest_of(kBasic, headers) != digest_of(kBasic));

  auto const files{ envy::tree_hash(kBasic, headers).files };
  CHECK(files == 2);  // lib.h and detail/impl.h; the 'include' dir itself is not a file
}

TEST_CASE("tree_filter exclude list removes what it names") {
  envy::tree_filter const no_tests{ .exclude = { "src/*_test.c" } };
  CHECK(digest_of(kBasic, no_tests) == oracle_digest(kBasic, no_tests));
  CHECK(envy::tree_hash(kBasic, no_tests).files == envy::tree_hash(kBasic).files - 1);
}

TEST_CASE("tree_filter exclude beats include when both match") {
  envy::tree_filter const both{ .include = { "src/**" }, .exclude = { "src/lib_test.c" } };
  auto const entries{ envy::tree_list(kBasic, both) };
  // "src/**" takes the directory as well as its contents, so what is left is the
  // directory plus the one file the exclude did not name.
  REQUIRE(entries.size() == 2);
  CHECK(entries[0].relpath == "src");
  CHECK(entries[1].relpath == "src/lib.c");
}

TEST_CASE("tree_filter selects a whole subtree from a literal directory name") {
  envy::tree_filter const literal{ .include = { "include" } };
  CHECK(digest_of(kBasic, literal) == digest_of(kBasic, { .include = { "include/**" } }));
}

TEST_CASE("tree_filter '**' spans components") {
  envy::tree_filter const headers{ .include = { "**/*.h" } };
  auto const entries{ envy::tree_list(kBasic, headers) };
  REQUIRE(entries.size() == 2);
  CHECK(entries[0].relpath == "include/detail/impl.h");
  CHECK(entries[1].relpath == "include/lib.h");
}

TEST_CASE("tree_filter that matches nothing yields the empty-selection digest") {
  envy::tree_filter const nothing{ .include = { "no/such/path" } };
  auto const r{ envy::tree_hash(kBasic, nothing) };
  CHECK(r.files == 0);
  CHECK(r.bytes == 0);
  CHECK(envy::tree_list(kBasic, nothing).empty());
  // Distinct from hashing everything, and stable, so a typo'd VENDOR list cannot
  // masquerade as an up-to-date vendor copy.
  CHECK(hex(r.digest) != digest_of(kBasic));
  CHECK(hex(r.digest) ==
        hex(envy::tree_hash(kBasic, { .include = { "other/miss" } }).digest));
}

TEST_CASE("tree_filter reaches selected files under an unselected directory") {
  // The walk must descend into directories the filter rejects. If it pruned them, a
  // VENDOR list naming only leaf files would come back empty.
  envy::tree_filter const deep{ .include = { "include/detail/**" } };
  auto const entries{ envy::tree_list(kBasic, deep) };
  REQUIRE(entries.size() == 2);  // the 'detail' directory and the header under it
  CHECK(entries[0].relpath == "include/detail");
  CHECK(entries[1].relpath == "include/detail/impl.h");
  CHECK(digest_of(kBasic, deep) == oracle_digest(kBasic, deep));
}

TEST_CASE("tree_hash rejects a root that is not a directory") {
  CHECK_THROWS_AS(envy::tree_hash(kBasic / "no-such-dir"), std::runtime_error);
  CHECK_THROWS_AS(envy::tree_hash(kBasic / "README"), std::runtime_error);
  CHECK_THROWS_AS(envy::tree_list(kBasic / "no-such-dir"), std::runtime_error);
}

TEST_CASE("tree_hash stats account for every file and directory") {
  envy::tree_hash_stats stats;
  auto const result{ envy::tree_hash(kBasic, {}, 4, &stats) };

  CHECK(hex(result.digest) == digest_of(kBasic));  // measuring must not change the answer
  CHECK(stats.threads == 4);

  std::uint64_t dirs{ 0 };
  for (auto const &e : envy::tree_list(kBasic)) {
    if (e.kind == envy::tree_entry_kind::DIRECTORY) { ++dirs; }
  }
  CHECK(stats.dirs == dirs + 1);  // every subdirectory, plus the root itself

  // Per-worker tallies are the balance report, so they have to add up to the whole.
  REQUIRE(stats.files_per_worker.size() == 4);
  REQUIRE(stats.bytes_per_worker.size() == 4);
  auto const sum{ [](std::vector<std::uint64_t> const &v) {
    return std::accumulate(v.begin(), v.end(), std::uint64_t{ 0 });
  } };
  CHECK(sum(stats.files_per_worker) == result.files);
  CHECK(sum(stats.bytes_per_worker) == result.bytes);
  CHECK(stats.wall_ns > 0);
}

TEST_CASE("tree_hash leaves stats alone when none were asked for") {
  // The timers are per-file clock reads, so a caller that passes nullptr must pay for
  // none of them -- and must still get the same digest.
  CHECK(hex(envy::tree_hash(kBasic, {}, 4, nullptr).digest) == digest_of(kBasic));
}

TEST_CASE("tree_hash paths are '/'-joined on every platform") {
  for (auto const &e : envy::tree_list(kBasic)) {
    CHECK(e.relpath.find('\\') == std::string::npos);
  }
}
