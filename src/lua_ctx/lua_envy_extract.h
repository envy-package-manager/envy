#pragma once

#include "extract.h"

#include "sol/sol.hpp"

#include <string>

namespace envy {

// Install envy.extract() and envy.extract_all() into the envy table
void lua_envy_extract_install(sol::table &envy_table);

// { strip, only, archives } for envy.extract, envy.extract_all and a table STAGE; errors
// read "<context>: ...".
extract_options lua_envy_extract_parse_opts(sol::table const &opts,
                                            std::string const &context);

}  // namespace envy
