#pragma once

#include "util.h"

#include "sol/sol.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace envy {

// Machinery shared by envy.json and envy.toml.

// Push the metatable that marks a table as an array, so an empty one encodes as `[]`.
// Decoders tag every array they build with it; encoders honor it.
void codec_push_array_mt(lua_State *L);

// envy.json.array / envy.toml.array: tag `t` (or a new table) as an array; returns it.
sol::table codec_array(sol::this_state L, sol::optional<sol::table> t);

bool codec_utf8_valid(std::string_view s);

// The document argument of a decode: a Lua string, never a coerced number or nil.
std::string_view codec_text_arg(sol::stack_object text, char const *fn);

// Where a value sits, for errors; built only when one is raised.
using codec_where = function_ref<std::string()>;

struct codec_style {
  char const *fn;  // error prefix, e.g. "envy.json.encode"
  bool toml;       // TOML inline syntax, else JSON
  int indent;      // JSON: spaces per level, 0 = compact
};

// What a table encodes as: an array (tagged, or keys exactly 1..n; a tagged one may have
// holes in JSON, written as null) or an object with its string keys sorted, so output is
// deterministic. Throws on anything else.
struct codec_table {
  bool array;
  lua_Integer n;  // array length
  std::vector<std::string> keys;
};
codec_table codec_classify(lua_State *L,
                           int idx,
                           codec_style const &style,
                           codec_where where);

// Append the value at `idx`, walking nested tables on a heap stack. `visiting` indexes a
// table of the tables open around it (cycle guard); `where` is its path, for errors.
void codec_encode(lua_State *L,
                  int idx,
                  int visiting,
                  codec_style const &style,
                  std::string const &where,
                  std::string &out);

// Append a key: TOML bare when it can be, else a quoted string.
void codec_append_key(std::string &out,
                      std::string_view key,
                      codec_style const &style,
                      codec_where where);

[[noreturn]] void codec_fail(codec_style const &style,
                             std::string_view what,
                             std::string const &where);

// Whole-file IO; relative paths resolve like envy.copy's. Errors name `fn` and the path.
std::vector<unsigned char> codec_read_file(sol::this_state L,
                                           std::string_view path,
                                           char const *fn);
void codec_write_file(sol::this_state L,
                      std::string_view path,
                      std::string_view content,
                      char const *fn);

}  // namespace envy
