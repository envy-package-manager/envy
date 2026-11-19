#include "lua_envy_decode.h"

#include "util.h"

#include "picojson.h"
#include "tomlc17.h"

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace envy {
namespace {

constexpr int kJsonMaxDepth{ 256 };

// picojson parse context that builds Lua values on the stack as it scans: no intermediate
// tree, and nesting is capped before recursion can exhaust the C stack.
class json_lua_ctx {
 public:
  explicit json_lua_ctx(lua_State *L) : L_{ L } {}
  bool too_deep() const { return too_deep_; }

  bool set_null() {
    lua_pushnil(L_);
    return true;
  }
  bool set_bool(bool b) {
    lua_pushboolean(L_, b);
    return true;
  }
  bool set_int64(std::int64_t i) {
    lua_pushinteger(L_, i);
    return true;
  }
  bool set_number(double d) {
    lua_pushnumber(L_, d);
    return true;
  }

  template <typename Iter>
  bool parse_string(picojson::input<Iter> &in) {
    str_.clear();
    if (!picojson::_parse_string(str_, in)) { return false; }
    lua_pushlstring(L_, str_.data(), str_.size());
    return true;
  }

  bool parse_array_start() { return open(); }
  bool parse_array_stop(std::size_t) { return true; }
  template <typename Iter>
  bool parse_array_item(picojson::input<Iter> &in, std::size_t i) {
    if (!nested(in)) { return false; }
    lua_rawseti(L_, -2, static_cast<lua_Integer>(i) + 1);
    return true;
  }

  bool parse_object_start() { return open(); }
  template <typename Iter>
  bool parse_object_item(picojson::input<Iter> &in, std::string const &key) {
    lua_pushlstring(L_, key.data(), key.size());
    if (!nested(in)) { return false; }
    lua_rawset(L_, -3);
    return true;
  }

 private:
  bool open() {
    if (!lua_checkstack(L_, 3)) { return false; }
    lua_createtable(L_, 0, 0);
    return true;
  }

  template <typename Iter>
  bool nested(picojson::input<Iter> &in) {
    if (depth_ == kJsonMaxDepth) {
      too_deep_ = true;
      return false;
    }
    ++depth_;
    bool const ok{ picojson::_parse(*this, in) };
    --depth_;
    return ok;
  }

  lua_State *L_;
  std::string str_;  // reused across strings
  int depth_{ 0 };
  bool too_deep_{ false };
};

sol::object json_decode(sol::this_state L, std::string_view text) {
  json_lua_ctx ctx{ L };
  picojson::input<char const *> in{ text.data(), text.data() + text.size() };
  if (!picojson::_parse(ctx, in)) {
    if (ctx.too_deep()) {
      throw_runtime_error({ "envy.json.decode: nesting deeper than ", kJsonMaxDepth });
    }
    throw_runtime_error({ "envy.json.decode: syntax error at line ", in.line() });
  }
  in.skip_ws();
  if (in.getc() != -1) {
    throw_runtime_error(
        { "envy.json.decode: unexpected trailing characters at line ", in.line() });
  }
  return sol::stack::pop<sol::object>(L);
}

void push_toml_datetime(lua_State *L, toml_datum_t const &d) {
  auto const &t{ d.u.ts };
  char buf[48];
  int n{ 0 };
  if (d.type != TOML_TIME) {
    n += std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", t.year, t.month, t.day);
  }
  if (d.type == TOML_DATETIME || d.type == TOML_DATETIMETZ) { buf[n++] = 'T'; }
  if (d.type != TOML_DATE) {
    n += std::snprintf(buf + n, sizeof buf - n, "%02d:%02d:%02d", t.hour, t.minute,
                       t.second);
    if (t.usec) {
      n += std::snprintf(buf + n, sizeof buf - n, ".%06d", static_cast<int>(t.usec));
      while (buf[n - 1] == '0') { --n; }  // ".500000" -> ".5"
    }
  }
  if (d.type == TOML_DATETIMETZ) {
    int const a{ std::abs(t.tz) };
    n += t.tz ? std::snprintf(buf + n, sizeof buf - n, "%c%02d:%02d", t.tz < 0 ? '-' : '+',
                              a / 60, a % 60)
              : std::snprintf(buf + n, sizeof buf - n, "Z");
  }
  lua_pushlstring(L, buf, static_cast<std::size_t>(n));
}

// tomlc17 caps nesting (30 brackets, 30 braces, 10 key parts): recursion stays shallow.
void push_toml(lua_State *L, toml_datum_t const &d) {
  if (!lua_checkstack(L, 3)) {
    throw_runtime_error({ "envy.toml.decode: Lua stack exhausted" });
  }
  switch (d.type) {
    case TOML_STRING:
      lua_pushlstring(L, d.u.str.ptr, static_cast<std::size_t>(d.u.str.len));
      return;
    case TOML_INT64: lua_pushinteger(L, d.u.int64); return;
    case TOML_FP64: lua_pushnumber(L, d.u.fp64); return;
    case TOML_BOOLEAN: lua_pushboolean(L, d.u.boolean); return;
    case TOML_DATE:
    case TOML_TIME:
    case TOML_DATETIME:
    case TOML_DATETIMETZ: push_toml_datetime(L, d); return;
    case TOML_ARRAY:
      lua_createtable(L, d.u.arr.size, 0);
      for (int i{ 0 }; i < d.u.arr.size; ++i) {
        push_toml(L, d.u.arr.elem[i]);
        lua_rawseti(L, -2, i + 1);
      }
      return;
    case TOML_TABLE:
      lua_createtable(L, 0, d.u.tab.size);
      for (int i{ 0 }; i < d.u.tab.size; ++i) {
        lua_pushlstring(L, d.u.tab.key[i], static_cast<std::size_t>(d.u.tab.len[i]));
        push_toml(L, d.u.tab.value[i]);
        lua_rawset(L, -3);
      }
      return;
    case TOML_UNKNOWN: break;
  }
  lua_pushnil(L);  // a parsed tree never holds TOML_UNKNOWN
}

sol::object toml_decode(sol::this_state L, std::string_view text) {
  [[maybe_unused]] static bool const utf8_checked{ [] {  // once, before any parse
    toml_option_t opt{ toml_default_option() };
    opt.check_utf8 = true;
    toml_set_option(opt);
    return true;
  }() };

  if (text.size() > INT_MAX) {
    throw_runtime_error({ "envy.toml.decode: input too large" });
  }
  struct doc_t {
    toml_result_t r;
    ~doc_t() { toml_free(r); }
  } const doc{ toml_parse(text.data(), static_cast<int>(text.size())) };
  if (!doc.r.ok) { throw_runtime_error({ "envy.toml.decode: ", doc.r.errmsg }); }
  push_toml(L, doc.r.toptab);
  return sol::stack::pop<sol::object>(L);
}

}  // namespace

void lua_envy_decode_install(sol::table &envy_table) {
  // set_function, not assignment: sol pushes an assigned function pointer without the
  // trampoline that turns C++ exceptions into Lua errors, so pcall could not catch them.
  sol::table json{ envy_table.lua_state(), sol::create };
  json.set_function("decode", json_decode);
  envy_table["json"] = json;
  sol::table toml{ envy_table.lua_state(), sol::create };
  toml.set_function("decode", toml_decode);
  envy_table["toml"] = toml;
}

}  // namespace envy
