#include "blake3_util.h"

namespace envy {

blake3_t blake3_hash(void const *data, size_t length) {
  blake3_stream s;
  s.update(data, length);
  return s.finalize();
}

}  // namespace envy
