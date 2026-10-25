#include "version.h"

#include "nanosemver.h"

#include <string_view>

namespace envy {

namespace {

std::string_view trim(std::string_view s) {
  auto const start{ s.find_first_not_of(" \t\n\r") };
  if (start == std::string_view::npos) { return {}; }
  return s.substr(start, s.find_last_not_of(" \t\n\r") - start + 1);
}

}  // namespace

bool version_is_newer(std::string_view candidate, std::string_view current) {
  candidate = trim(candidate);
  current = trim(current);

  nsv_semver_t cand{};
  if (nsv_parse(&cand, candidate.data(), candidate.size()) != NSV_RET_OK) { return false; }

  nsv_semver_t cur{};
  if (nsv_parse(&cur, current.data(), current.size()) != NSV_RET_OK) { return true; }

  return nsv_cmp(&cand, &cur) == NSV_CMP_GT;
}

}  // namespace envy
