#include "libgit2_util.h"

#include <git2.h>

#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>

#include <filesystem>
#include <mutex>
#include <vector>
#endif

#ifdef __APPLE__
#include "ssl_certs_macos.h"

#include <unistd.h>
#endif

#include "util.h"

namespace envy {

#ifndef _WIN32
namespace {

std::once_flag g_ssl_certs_once;
bool g_ssl_certs_configured{ false };
std::unique_ptr<scoped_path_cleanup> g_ssl_cert_file_cleanup;

#ifdef __APPLE__
// Write certificate bundle to temp file with unique name via mkstemp.
// Returns scoped_path_cleanup that owns the temp file.
std::unique_ptr<scoped_path_cleanup> write_cert_bundle(
    std::vector<unsigned char> const &cert_data) {
  std::string path{
    (std::filesystem::temp_directory_path() / "envy-ca-certs-XXXXXX").string()
  };

  int const fd{ ::mkstemp(path.data()) };
  if (fd == -1) {
    throw_runtime_error({ "Failed to create temp file for CA certificates" });
  }

  // Immediately register cleanup so file is deleted even if fdopen/fwrite fails
  auto cleanup = std::make_unique<scoped_path_cleanup>(path);

  FILE *raw_file = ::fdopen(fd, "wb");
  if (!raw_file) {
    ::close(fd);
    throw_runtime_error({ "Failed to open temp file for CA certificates" });
  }
  file_ptr_t file{ raw_file };

  if (std::fwrite(cert_data.data(), 1, cert_data.size(), file.get()) != cert_data.size()) {
    throw_runtime_error({ "Failed to write CA certificates to temp file" });
  }

  return cleanup;
}

void configure_ssl_certs_macos() {
  auto cert_data{ extract_system_ca_certs() };
  auto cleanup{ write_cert_bundle(cert_data) };
  auto const &cert_path = cleanup->path();

  if (git_libgit2_opts(GIT_OPT_SET_SSL_CERT_LOCATIONS, cert_path.c_str(), nullptr) < 0) {
    throw_runtime_error({ "Failed to configure libgit2 SSL certificate location" });
  }

  // Transfer ownership to global only after success
  g_ssl_cert_file_cleanup = std::move(cleanup);
  g_ssl_certs_configured = true;
}
#else
// Linux: probe standard CA bundle locations
void configure_ssl_certs_linux() {
  static constexpr char const *ca_paths[] = {
    "/etc/ssl/certs/ca-certificates.crt",  // Debian/Ubuntu
    "/etc/pki/tls/certs/ca-bundle.crt",    // RHEL/CentOS/Fedora
    "/etc/ssl/ca-bundle.pem",              // OpenSUSE
    "/etc/pki/tls/cacert.pem",             // OpenELEC
  };

  struct stat st;
  for (auto const *path : ca_paths) {
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
      if (git_libgit2_opts(GIT_OPT_SET_SSL_CERT_LOCATIONS, path, nullptr) == 0) {
        g_ssl_certs_configured = true;
        return;
      }
    }
  }
}
#endif

void ensure_ssl_certs_configured() {
  std::call_once(g_ssl_certs_once, [] {
#ifdef __APPLE__
    configure_ssl_certs_macos();
#else
    configure_ssl_certs_linux();
#endif
  });
}

}  // namespace
#endif

libgit2_scope::libgit2_scope() {
  if (git_libgit2_init() < 0) { throw_runtime_error({ "Failed to initialize libgit2" }); }
}

libgit2_scope::~libgit2_scope() { git_libgit2_shutdown(); }

void libgit2_require_ssl_certs() {
#ifndef _WIN32
  ensure_ssl_certs_configured();
  if (!g_ssl_certs_configured) {
    throw_runtime_error(
        { "No CA certificate bundle found. Install ca-certificates package." });
  }
#endif
}

}  // namespace envy
