#pragma once

namespace envy {

class engine;

struct pkg;
void run_vendor_phase(pkg *p, engine &eng);

}  // namespace envy
