#pragma once

#include "util.h"

#include "blake3.h"

#include <array>
#include <cstddef>

namespace envy {

using blake3_t = std::array<unsigned char, 32>;

// The one wrapper over the upstream C API: a buffer, a file read in chunks, and a whole
// tree all fold through it. Holds the hasher by value, so instances are independent.
class blake3_stream : uncopyable {
 public:
  blake3_stream() { blake3_hasher_init(&h_); }

  void update(void const *data, size_t length) { blake3_hasher_update(&h_, data, length); }

  // Const: finalizing does not consume the state, and further updates are legal.
  blake3_t finalize() const {
    blake3_t digest;
    blake3_hasher_finalize(&h_, digest.data(), digest.size());
    return digest;
  }

 private:
  mutable blake3_hasher h_;
};

// One-shot over one buffer, for the many callers hashing a short string.
blake3_t blake3_hash(void const *data, size_t length);

}  // namespace envy
