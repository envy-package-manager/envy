#pragma once

#include "pkg.h"
#include "pkg_phase.h"

#include <optional>
#include <string>

namespace envy {

// One direct dependency edge: the provider, the identity the consumer's map keys it
// under, and the needed_by that edge carries.
struct direct_dependency {
  pkg *p;
  std::string identity;
  pkg_phase needed_by;
};

// The consumer's own dependency matching `query` -- exactly, or fuzzily by name,
// namespace.name, or name@revision -- with the earliest needed_by winning and ties
// broken by identity. Only direct edges answer: an edge is what ordered the
// provider's payload for *this* consumer, and a provider reached only through
// someone else's edge has nothing ordering it here.
std::optional<direct_dependency> find_direct_dependency(pkg *from,
                                                        std::string const &query);

}  // namespace envy
