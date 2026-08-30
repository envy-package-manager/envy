#pragma once

#include "zlib.h"

// zconf.h does `#if defined(_WINDOWS) && !defined(WINDOWS) -> #define WINDOWS`, and MSVC
// builds of a WIN32 executable define _WINDOWS. That empty macro turns every
// envy::platform_id::WINDOWS into `platform_id::` -- a syntax error visible only on the
// Windows CI. zlib reads WINDOWS solely while zconf.h is being processed, so dropping it
// afterwards is safe; include this instead of <zlib.h> anywhere in envy.
#undef WINDOWS
