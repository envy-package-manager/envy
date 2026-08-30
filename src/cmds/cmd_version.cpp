#include "cmd_version.h"

#include "embedded_licenses.h"
#include "platform.h"

#include "archive.h"
#include "aws/core/Version.h"
#include "aws/crt/Api.h"
#include "blake3.h"
#include "bzlib.h"
#include "cli_parse.h"
#if !defined(_WIN32)
#include "curl/curl.h"
#endif
#include "git2.h"
#include "lzma.h"
#ifndef _WIN32
#include "mbedtls/version.h"
#endif
#include "libssh2.h"
#include "semver.hpp"
#include "sol/sol.hpp"
#include "tui.h"
#include "util.h"
#include "zlib_compat.h"
#include "zstd.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef ENVY_VERSION_STR
#error "ENVY_VERSION_STR must be defined by the build system"
#endif

namespace envy {

cli_cmd &cmd_version::register_cli(cli_cmd &app, cfg &c) {
  auto &sub{ app.sub("version", "Show version information") };
  sub.flag("--licenses", c.show_licenses, "Print all licenses");
  return sub;
}

namespace {

void print_licenses() {
  std::string const text{ util_inflate_resource(embedded::kLicenses) };
  if (std::fwrite(text.data(), 1, text.size(), stdout) != text.size()) {
    tui::error("Failed to write licenses to stdout");
  }
}

}  // namespace

cmd_version::cmd_version(cmd_version::cfg cfg,
                         std::optional<std::filesystem::path> const & /*cli_cache_root*/)
    : cfg_{ std::move(cfg) } {}

void cmd_version::execute() {
  tui::info("envy version %s (%s)",
            ENVY_VERSION_STR,
            platform::get_exe_path().string().c_str());
  tui::info("");
  tui::info("Third-party component versions:");

  int git_major{ 0 };
  int git_minor{ 0 };
  int git_revision{ 0 };
  git_libgit2_version(&git_major, &git_minor, &git_revision);
  tui::info("  libgit2: %d.%d.%d", git_major, git_minor, git_revision);

#if !defined(_WIN32)
  curl_version_info_data const *curl_info{ curl_version_info(CURLVERSION_NOW) };
  std::vector<std::string> curl_features;
  if (curl_info->features & CURL_VERSION_ZSTD) { curl_features.push_back("zstd"); }
  if (curl_info->features & CURL_VERSION_BROTLI) { curl_features.push_back("brotli"); }
  if (curl_info->features & CURL_VERSION_LIBZ) { curl_features.push_back("zlib"); }
  if (!curl_features.empty()) {
    std::string features;
    for (size_t i{ 0 }; i < curl_features.size(); ++i) {
      if (i > 0) features.append(", ");
      features.append(curl_features[i]);
    }
    tui::info("  libcurl: %s (%s)", curl_info->version, features.c_str());
  } else {
    tui::info("  libcurl: %s", curl_info->version);
  }
#else
  tui::info("  HTTP: WinINet (system)");
#endif

  tui::info("  libssh2: %s", LIBSSH2_VERSION);

#ifndef _WIN32
  std::array<char, 32> mbedtls_version{};
  mbedtls_version_get_string_full(mbedtls_version.data());
  tui::info("  mbedTLS: %s", mbedtls_version.data());
#endif

  tui::info("  libarchive: %s", archive_version_details());
  tui::info("  Lua: %s", LUA_RELEASE);
  tui::info("  Sol2: %s (%s)", SOL_VERSION_STRING, SOL2_GIT_SHA_SHORT);
  tui::info("  BLAKE3: %s", BLAKE3_VERSION_STRING);
  tui::info("  zlib: %s", zlibVersion());
  tui::info("  bzip2: %s", BZ2_bzlibVersion());
  tui::info("  zstd: %s", ZSTD_versionString());
  tui::info("  liblzma: %s", lzma_version_string());
  auto const *aws_sdk_version{ Aws::Version::GetVersionString() };
  tui::info("  AWS SDK for C++: %s", aws_sdk_version);

  Aws::Crt::ApiHandle crt_handle;
  auto const crt_version{ crt_handle.GetCrtVersion() };
  tui::info("  AWS CRT: %u.%u.%u",
            static_cast<unsigned>(crt_version.major),
            static_cast<unsigned>(crt_version.minor),
            static_cast<unsigned>(crt_version.patch));

  tui::info("  Semver: %d.%d.%d",
            SEMVER_VERSION_MAJOR,
            SEMVER_VERSION_MINOR,
            SEMVER_VERSION_PATCH);
  tui::info("  picojson: %s", ENVY_PICOJSON_VERSION);

  if (cfg_.show_licenses) {
    tui::info("");
    print_licenses();
  }
}

}  // namespace envy
