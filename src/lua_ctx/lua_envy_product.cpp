#include "lua_envy_product.h"

#include "lua_phase_context.h"
#include "pkg.h"
#include "pkg_phase.h"
#include "product_expand.h"
#include "product_util.h"
#include "util.h"

#include <stdexcept>
#include <string>

namespace envy {

void lua_envy_product_install(sol::table &envy_table) {
  // envy.product(name) -> path_or_value_string
  envy_table["product"] = [](std::string const &product_name,
                             sol::this_state L) -> std::string {
    phase_context const *ctx{ lua_phase_context_get(L) };
    pkg *const p{ ctx ? ctx->p : nullptr };
    if (!p) {
      throw_runtime_error({ "envy.product: not in phase context (missing pkg)" });
    }
    pkg_phase const phase{ p->current_phase.load() };
    product_grant const grant{ product_authorize(p, phase, product_name, "envy.product") };
    if (product_util_is_command(grant.provider, product_name)) {
      std::string const msg{ "envy.product: product '" + product_name +
                             "' is a command, not a path; run it as '{{" + product_name +
                             "}}' in phase text" };
      product_trace(p, product_name, phase, grant, false, msg);
      throw std::runtime_error{ msg };
    }
    std::string const value{ product_util_resolve(
        grant.provider,
        product_name,
        [&](pkg *r) { return product_payload_dir(p, r); }) };
    product_trace(p, product_name, phase, grant, true, value);
    return value;
  };
}

}  // namespace envy
