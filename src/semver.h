#pragma once

#include "nanosemver.h"

#include <string>
#include <string_view>

namespace envy {

// A failure's NSV_DIAG account ("bad range at column 9: ...", the input, a caret) lands in
// *why, '\n'-led to append to an error; empty on success, dropped if why is null. All envy
// parses come through here: the first call installs nanosemver's process-wide print hook.
int semver_parse(nsv_semver_t &out, std::string_view s, std::string *why = nullptr);

// No flags: a pre-release is a version below its release (npm's includePrerelease).
// 1 if v satisfies range, 0 if not, < 0 if range is malformed.
int semver_range_match(std::string_view range,
                       nsv_semver_t const &v,
                       std::string *why = nullptr);

}  // namespace envy
