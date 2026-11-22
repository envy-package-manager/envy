#pragma once

#include "util.h"

namespace envy {

// Initializes libgit2 on first call, from any thread; call before any other libgit2 API.
// Lazy because its TLS setup loads CA certificates, which commands without git don't need.
void libgit2_ensure();

// Shuts libgit2 down at scope exit, if libgit2_ensure ever initialized it.
struct libgit2_scope : unmovable {
  libgit2_scope() = default;
  ~libgit2_scope();
};

// Initializes libgit2, then on Linux and macOS throws if SSL certificates required for
// HTTPS git operations are not found (no-op on Windows). Call before HTTPS git fetches.
void libgit2_require_ssl_certs();

}  // namespace envy
