#pragma once

#include "envy_release.h"
#include "fetch.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#ifndef ENVY_VERSION_STR
#error "ENVY_VERSION_STR must be defined by the build system"
#endif

namespace envy {

// Sent on every fetch, by whichever backend the platform builds. A `0.0` placeholder is
// precisely what server-side abuse heuristics flag, so carry the version the build
// stamped; the URL gives an operator who wonders who is knocking somewhere to look.
inline std::string const &fetch_user_agent() {
  static std::string const agent{ "envy/" ENVY_VERSION_STR " (+" +
                                  std::string{ kEnvyUpstreamRepoUrl } + ")" };
  return agent;
}

std::filesystem::path fetch_http_download(std::string_view url,
                                          std::filesystem::path const &destination,
                                          fetch_progress_cb_t const &progress,
                                          std::optional<std::string> const &post_data);

}  // namespace envy
