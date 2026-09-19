#include "file_read.h"

#include "blake3_util.h"
#include "sha256.h"
#include "util.h"

#include "doctest.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path const kFixtures{ "test_data/tree_hash/basic" };

// Two Windows traps in one line. A narrow literal becomes a path through the active code
// page, so it must be char8_t; and MSVC reads a BOM-less source file in that code page
// too, so the name is spelled with universal character names rather than its own bytes.
fs::path u8path(char8_t const *name) { return fs::path{ std::u8string{ name } }; }

// Everything file_read_chunks handed over, concatenated, plus how it was chunked.
struct capture {
  std::vector<unsigned char> bytes;
  std::vector<std::size_t> chunks;

  envy::file_chunk_sink sink() {
    return [this](void const *data, std::size_t n) {
      auto const *p{ static_cast<unsigned char const *>(data) };
      bytes.insert(bytes.end(), p, p + n);
      chunks.push_back(n);
    };
  }
};

std::vector<unsigned char> read_via_stdio(fs::path const &p) {
  return envy::util_load_file(p);
}

}  // namespace

TEST_CASE("file_read_chunks delivers a file's bytes in order") {
  for (auto const *name : { "README", "bin/data.bin", "src/lib.c" }) {
    capture c;
    auto const size{ envy::file_read_chunks(kFixtures / name, c.sink()) };
    auto const want{ read_via_stdio(kFixtures / name) };

    CHECK(size == want.size());
    CHECK(c.bytes == want);
  }
}

TEST_CASE("file_read_chunks reports an empty file as zero bytes and no chunks") {
  capture c;
  CHECK(envy::file_read_chunks(kFixtures / "empty", c.sink()) == 0);
  CHECK(c.bytes.empty());
  CHECK(c.chunks.empty());
}

TEST_CASE("file_read_chunks handles a non-ASCII path") {
  capture c;
  auto const path{ kFixtures / "docs" / u8path(u8"\u00fcn\u00efcode.txt") };
  CHECK(envy::file_read_chunks(path, c.sink()) == read_via_stdio(path).size());
  CHECK(c.bytes == read_via_stdio(path));
}

TEST_CASE("file_read_chunks sized and unsized forms agree") {
  auto const path{ kFixtures / "bin/data.bin" };
  capture unsized, sized;
  auto const size{ envy::file_read_chunks(path, unsized.sink()) };
  envy::file_read_chunks(envy::file_native_path(path), size, sized.sink());
  CHECK(sized.bytes == unsized.bytes);
}

TEST_CASE("file_read_chunks throws on a path that is not a readable file") {
  capture c;
  CHECK_THROWS_AS(envy::file_read_chunks(kFixtures / "no-such-file", c.sink()),
                  std::runtime_error);
  // A directory has no contents to stream, and silently returning none would let a
  // caller hash "nothing" and record it as the answer.
  CHECK_THROWS_AS(envy::file_read_chunks(kFixtures, c.sink()), std::runtime_error);
}

TEST_CASE("file_read_chunks feeds a hasher the same bytes the file holds") {
  // The property both digests are built on: whatever the reader's chunking turns out to
  // be, a hasher fed those chunks lands on the buffer's own digest.
  auto const path{ kFixtures / "bin/data.bin" };
  auto const bytes{ read_via_stdio(path) };

  capture c;
  envy::blake3_stream streamed;
  envy::file_read_chunks(path, [&](void const *data, std::size_t n) {
    streamed.update(data, n);
    c.sink()(data, n);
  });

  CHECK(c.bytes == bytes);
  CHECK(streamed.finalize() == envy::blake3_hash(bytes.data(), bytes.size()));

  // Every chunk carried something, and they add up to the file.
  CHECK_FALSE(c.chunks.empty());
  CHECK(std::ranges::none_of(c.chunks, [](std::size_t n) { return n == 0; }));
  CHECK(std::accumulate(c.chunks.begin(), c.chunks.end(), std::size_t{ 0 }) ==
        bytes.size());
}

TEST_CASE("sha256 reads through the shared reader") {
  // Both digests read through file_read_chunks, so the per-file answer must follow
  // from the bytes that reader hands out.
  auto const path{ kFixtures / "bin/data.bin" };
  auto const bytes{ read_via_stdio(path) };

  std::uint64_t reported{ 0 };
  auto const progress_total{ [&](std::uint64_t done, std::uint64_t total) {
    CHECK(total == bytes.size());
    reported = done;
  } };
  CHECK(envy::sha256(path, progress_total) == envy::sha256(path));
  CHECK(reported == bytes.size());
}

TEST_CASE("sha256 of an empty file still reports one terminal progress frame") {
  int calls{ 0 };
  envy::sha256(kFixtures / "empty", [&](std::uint64_t done, std::uint64_t total) {
    CHECK(done == 0);
    CHECK(total == 0);
    ++calls;
  });
  CHECK(calls == 1);
}
