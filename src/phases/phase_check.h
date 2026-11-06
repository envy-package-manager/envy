#pragma once

#include <string>

namespace envy {

class engine;
struct pkg;

void run_check_phase(pkg *p, engine &eng);

// Hex BLAKE3 of the canonical key plus every resolved weak dependency key: the hash that
// names the package's cache entry, and the one product prediction must agree with.
std::string pkg_cache_hash(pkg const &p);

}  // namespace envy
