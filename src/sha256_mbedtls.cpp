#include "sha256.h"

#include "file_read.h"
#include "util.h"

#include "mbedtls/sha256.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

namespace envy {

sha256_t sha256(std::filesystem::path const &file_path,
                byte_progress_cb_t const &progress) {
  if (!std::filesystem::exists(file_path)) {
    throw std::runtime_error("sha256: file does not exist: " + file_path.string());
  }

  // A length is what makes this a bar rather than a spinner, so it is read before a byte
  // of content is, and handed straight to the shared reader -- one stat, not two.
  std::error_code size_ec;
  auto const total{ std::filesystem::file_size(file_path, size_ec) };
  if (size_ec) {
    throw std::runtime_error("sha256: cannot size " + file_path.string() + ": " +
                             size_ec.message());
  }

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);

  std::unique_ptr<decltype(ctx), decltype(&mbedtls_sha256_free)> ctx_scope(
      &ctx,
      &mbedtls_sha256_free);

  if (mbedtls_sha256_starts(&ctx, 0)) {
    throw std::runtime_error("sha256: mbedtls_sha256_starts failed");
  }

  // One file-reading path in the process: the same platform-native reader the subtree
  // hash uses, so read sizing, readahead and short-read handling are decided once.
  std::uint64_t hashed{ 0 };
  file_read_chunks(
      file_native_path(file_path),
      total,
      [&](void const *data, std::size_t n) {
        if (mbedtls_sha256_update(&ctx, static_cast<unsigned char const *>(data), n)) {
          throw std::runtime_error("sha256: mbedtls_sha256_update failed");
        }
        hashed += n;
        if (progress) { progress(hashed, total); }
      });

  // An empty file reports nothing above, so its row still gets one terminal frame.
  if (progress) { progress(hashed, total); }

  sha256_t digest{};
  if (mbedtls_sha256_finish(&ctx, digest.data())) {
    throw std::runtime_error("sha256: mbedtls_sha256_finish failed");
  }

  return digest;
}

void sha256_verify(std::string const &expected_hex, sha256_t const &actual_hash) {
  if (expected_hex.size() != 64) {
    throw std::runtime_error(
        "sha256_verify: expected hex string must be 64 characters, got " +
        std::to_string(expected_hex.size()));
  }

  auto const expected_bytes{ util_hex_to_bytes(expected_hex) };
  if (expected_bytes.size() != 32) {
    throw std::runtime_error("sha256_verify: hex conversion produced wrong size: " +
                             std::to_string(expected_bytes.size()));
  }

  if (std::memcmp(expected_bytes.data(), actual_hash.data(), 32) != 0) {
    throw std::runtime_error("SHA256 mismatch: expected " + expected_hex + " but got " +
                             util_bytes_to_hex(actual_hash.data(), actual_hash.size()));
  }
}

}  // namespace envy
