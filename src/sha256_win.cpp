#include "sha256.h"

#include "file_read.h"
#include "platform.h"
#include "util.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

#include <bcrypt.h>

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

  BCRYPT_ALG_HANDLE alg_handle{ nullptr };
  NTSTATUS status{
    BCryptOpenAlgorithmProvider(&alg_handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0)
  };
  if (!BCRYPT_SUCCESS(status)) {
    throw std::runtime_error("sha256: BCryptOpenAlgorithmProvider failed");
  }

  auto alg_deleter = [](BCRYPT_ALG_HANDLE h) { BCryptCloseAlgorithmProvider(h, 0); };
  std::unique_ptr<void, decltype(alg_deleter)> alg_scope(alg_handle, alg_deleter);

  BCRYPT_HASH_HANDLE hash_handle{ nullptr };
  status = BCryptCreateHash(alg_handle, &hash_handle, nullptr, 0, nullptr, 0, 0);
  if (!BCRYPT_SUCCESS(status)) {
    throw std::runtime_error("sha256: BCryptCreateHash failed");
  }

  auto hash_deleter = [](BCRYPT_HASH_HANDLE h) { BCryptDestroyHash(h); };
  std::unique_ptr<void, decltype(hash_deleter)> hash_scope(hash_handle, hash_deleter);

  // One file-reading path in the process: the same platform-native reader the subtree
  // hash uses, so read sizing, readahead and short-read handling are decided once.
  std::uint64_t hashed{ 0 };
  file_read_chunks(
      file_native_path(file_path),
      total,
      [&](void const *data, std::size_t n) {
        auto const hash_status{ BCryptHashData(
            hash_handle,
            const_cast<PUCHAR>(static_cast<unsigned char const *>(data)),
            static_cast<ULONG>(n),
            0) };
        if (!BCRYPT_SUCCESS(hash_status)) {
          throw std::runtime_error("sha256: BCryptHashData failed");
        }
        hashed += n;
        if (progress) { progress(hashed, total); }
      });

  // An empty file reports nothing above, so its row still gets one terminal frame.
  if (progress) { progress(hashed, total); }

  sha256_t digest{};
  status =
      BCryptFinishHash(hash_handle, digest.data(), static_cast<ULONG>(digest.size()), 0);
  if (!BCRYPT_SUCCESS(status)) {
    throw std::runtime_error("sha256: BCryptFinishHash failed");
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
