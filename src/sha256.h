#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace envy {

using sha256_t = std::array<unsigned char, 32>;

// Bytes hashed so far and the file's length, reported once per read chunk. A file's size
// is always known, so a caller drawing this is always drawing a determinate bar.
using byte_progress_cb_t = std::function<void(std::uint64_t done, std::uint64_t total)>;

sha256_t sha256(std::filesystem::path const &file_path,
                byte_progress_cb_t const &progress = {});

// Verify SHA256 hash matches expected hex string (case-insensitive)
// Throws std::runtime_error with detailed message if mismatch
void sha256_verify(std::string const &expected_hex, sha256_t const &actual_hash);

}  // namespace envy
