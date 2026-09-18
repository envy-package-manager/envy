#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>

namespace envy {

// The one path that reads a file's bytes for hashing. Both `sha256` and `tree_hash` go
// through it, so read sizing, readahead and short-read handling are decided once.

using file_chunk_sink = std::function<void(void const *, std::size_t)>;

// The OS's own encoding: UTF-16 on Windows, bytes elsewhere. A tree walk stays in this
// form from readdir to open.
using file_native_string = std::filesystem::path::string_type;

// `path` adapted for native IO; on Windows this adds the \\?\ prefix that lifts MAX_PATH.
file_native_string file_native_path(std::filesystem::path const &path);

// Read `size` bytes in order into `sink`, several reads in flight, per-thread buffer.
// Throws naming `path` on a read error or a file shorter than `size`.
void file_read_chunks(file_native_string const &path,
                      std::uint64_t size,
                      file_chunk_sink const &sink);

// Same, for a caller holding a path but no size. Returns the bytes read.
std::uint64_t file_read_chunks(std::filesystem::path const &path,
                               file_chunk_sink const &sink);

}  // namespace envy
