#pragma once

#include "util.h"

#include <string_view>

namespace envy {

// Initializes libgit2 on first call, from any thread; call before any other libgit2 API.
// Lazy, so commands without git skip libgit2's global setup.
void libgit2_ensure();

// Shuts libgit2 down at scope exit, if libgit2_ensure ever initialized it.
struct libgit2_scope : unmovable {
  libgit2_scope() = default;
  ~libgit2_scope();
};

// Before connecting to `url` -- git_remote_url, after insteadOf -- loads CA certificates
// off Windows: required for https, best effort for http, which may redirect to https.
void libgit2_ca_certs_for(std::string_view url);

}  // namespace envy
