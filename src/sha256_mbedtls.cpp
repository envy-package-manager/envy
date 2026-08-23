#include "sha256.h"

#include "util.h"

#include "mbedtls/sha256.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace envy {

sha256_t sha256(std::filesystem::path const &file_path,
                byte_progress_cb_t const &progress) {
  if (!std::filesystem::exists(file_path)) {
    throw std::runtime_error("sha256: file does not exist: " + file_path.string());
  }

  // A length is what makes this a bar. Without one, no per-chunk report goes out at all —
  // only the terminal one below, which also gives an empty file a row.
  std::error_code size_ec;
  auto const total{ std::filesystem::file_size(file_path, size_ec) };
  bool const total_known{ !size_ec };
  std::uint64_t hashed{ 0 };

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);

  std::unique_ptr<decltype(ctx), decltype(&mbedtls_sha256_free)> ctx_scope(
      &ctx,
      &mbedtls_sha256_free);

  if (mbedtls_sha256_starts(&ctx, 0)) {
    throw std::runtime_error("sha256: mbedtls_sha256_starts failed");
  }

  file_ptr_t file{ util_open_file(file_path, "rb") };
  if (!file) {
    throw std::runtime_error("sha256: failed to open file: " + file_path.string());
  }

  std::vector<unsigned char> buffer(1024 * 1024);
  while (true) {
    auto const read_bytes{
      std::fread(buffer.data(), sizeof(unsigned char), buffer.size(), file.get())
    };

    if (read_bytes > 0) {
      if (mbedtls_sha256_update(&ctx, buffer.data(), read_bytes)) {
        throw std::runtime_error("sha256: mbedtls_sha256_update failed");
      }
      hashed += read_bytes;
      if (progress && total_known) { progress(hashed, total); }
    }

    if (read_bytes < buffer.size()) {
      if (std::ferror(file.get())) { throw std::runtime_error("sha256: fread failed"); }
      break;
    }
  }

  if (progress) { progress(hashed, total_known ? total : hashed); }

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
