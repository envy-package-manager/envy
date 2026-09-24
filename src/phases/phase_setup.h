#pragma once

#include "tui.h"

#include <string>

namespace envy {

class engine;
struct pkg;

void run_setup_phase(pkg *p, engine &eng);

// One pair of `p` (the declaring package): double-check lock, CHECK gate,
// INSTALL against the host. Shell output lands in `section`, labeled with
// `p`'s identity. Called from pair-task step callbacks (engine).
void run_setup_pair(pkg *p,
                    engine &eng,
                    std::string const &name,
                    tui::section_handle section);

}  // namespace envy
