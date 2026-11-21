#include "lua_envy_codec_util.h"

#include "util.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>

namespace envy {
namespace {

constexpr char kArrayMt[]{ "envy.array" };

bool is_tagged_array(lua_State *L, int idx) {
  if (!lua_getmetatable(L, idx)) { return false; }
  luaL_getmetatable(L, kArrayMt);
  bool const tagged{ lua_rawequal(L, -1, -2) != 0 };
  lua_pop(L, 2);
  return tagged;
}

// Escapes valid in both a JSON string and a TOML basic string; UTF-8 passes through.
void append_quoted(std::string &out, std::string_view s) {
  out += '"';
  for (char const ch : s) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (auto const c{ static_cast<unsigned char>(ch) }; c < 0x20 || c == 0x7f) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04X", c);
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  out += '"';
}

void append_string(std::string &out,
                   std::string_view s,
                   codec_style const &style,
                   codec_where where) {
  if (!codec_utf8_valid(s)) { codec_fail(style, "string is not valid UTF-8", where()); }
  append_quoted(out, s);
}

void append_float(std::string &out, double d) {  // shortest text that reads back as d
  char buf[32];
  for (int prec{ 15 }; prec <= 17; ++prec) {
    std::snprintf(buf, sizeof buf, "%.*g", prec, d);
    if (std::strtod(buf, nullptr) == d) { break; }
  }
  out += buf;
  if (!std::strpbrk(buf, ".e")) { out += ".0"; }  // keep it a float when read back
}

}  // namespace

void codec_push_array_mt(lua_State *L) { luaL_newmetatable(L, kArrayMt); }

sol::table codec_array(sol::this_state L, sol::optional<sol::table> t) {
  sol::table tbl{ t ? *t : sol::table{ L, sol::create } };
  tbl.push();
  codec_push_array_mt(L);
  lua_setmetatable(L, -2);
  lua_pop(L, 1);
  return tbl;
}

bool codec_utf8_valid(std::string_view s) {
  auto const *p{ reinterpret_cast<unsigned char const *>(s.data()) };
  auto const *const end{ p + s.size() };
  while (p < end) {
    unsigned const c{ *p };
    if (c < 0x80) {
      ++p;
      continue;
    }
    int const n{ c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 ? 1 : 0 };  // continuations
    if (n == 0 || c > 0xf4 || end - p <= n) { return false; }
    std::uint32_t cp{ c & (0x3fu >> n) };
    for (int i{ 1 }; i <= n; ++i) {
      if ((p[i] & 0xc0) != 0x80) { return false; }
      cp = (cp << 6) | (p[i] & 0x3fu);
    }
    if ((n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff)) {
      return false;  // overlong, out of range, or a surrogate
    }
    p += n + 1;
  }
  return true;
}

void codec_fail(codec_style const &style,
                std::string_view what,
                std::string const &where) {
  if (where.empty()) { throw_runtime_error({ style.fn, ": ", what }); }
  throw_runtime_error({ style.fn, ": ", what, " at ", where });
}

codec_table codec_classify(lua_State *L,
                           int idx,
                           codec_style const &style,
                           codec_where where) {
  idx = lua_absindex(L, idx);
  bool const tagged{ is_tagged_array(L, idx) };
  codec_table t{ tagged, 0, {} };
  lua_Integer max{ 0 };
  lua_pushnil(L);
  while (lua_next(L, idx)) {
    lua_pop(L, 1);  // the value; the key stays for lua_next
    if (lua_type(L, -1) == LUA_TSTRING) {
      std::size_t len;
      char const *const k{ lua_tolstring(L, -1, &len) };
      t.keys.emplace_back(k, len);
    } else if (lua_isinteger(L, -1) && lua_tointeger(L, -1) >= 1) {
      ++t.n;
      max = std::max(max, lua_tointeger(L, -1));
    } else {
      lua_pop(L, 1);
      codec_fail(style, "table keys must be strings or array indices", where());
    }
  }
  if ((t.n || tagged) && !t.keys.empty()) {
    codec_fail(style, "table mixes array indices and string keys", where());
  }
  if (t.n != max) {  // a tagged JSON array's holes are the nulls it was decoded from
    if (style.toml || !tagged) { codec_fail(style, "array has holes", where()); }
    t.n = max;
  }
  t.array = tagged || t.n > 0;
  util_sort(t.keys, std::less<>{});
  return t;
}

void codec_append_key(std::string &out,
                      std::string_view key,
                      codec_style const &style,
                      codec_where where) {
  bool const bare{ style.toml && !key.empty() &&
                   std::all_of(key.begin(), key.end(), [](char c) {
                     return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '-';
                   }) };
  if (bare) {
    out += key;
  } else {
    append_string(out, key, style, where);
  }
}

void codec_encode(lua_State *L,
                  int idx,
                  int visiting,
                  codec_style const &style,
                  std::string const &where,
                  std::string &out) {
  struct frame {
    int idx;  // the container, held on the Lua stack while open
    codec_table t;
    std::size_t next;  // elements started; arrays: also the 1-based index of the last
  };
  std::vector<frame> frames;

  auto const path{ [&] {  // where the value being written sits, for errors
    std::string p{ where };
    for (frame const &f : frames) {
      if (f.t.array) {
        p += '[' + std::to_string(f.next) + ']';
      } else {
        p += (p.empty() ? "" : ".") + f.t.keys[f.next - 1];
      }
    }
    return p;
  } };
  auto const newline{ [&](std::size_t depth) {
    if (style.toml || !style.indent) { return; }
    out += '\n';
    out.append(depth * static_cast<std::size_t>(style.indent), ' ');
  } };

  // Write the value on top of the stack. A non-empty table opens a frame and stays on
  // the stack; anything else is written whole and popped.
  auto const write_top{ [&] {
    switch (lua_type(L, -1)) {
      case LUA_TNIL:
        if (style.toml) { codec_fail(style, "TOML has no nil", path()); }
        out += "null";
        break;
      case LUA_TBOOLEAN: out += lua_toboolean(L, -1) ? "true" : "false"; break;
      case LUA_TNUMBER:
        if (lua_isinteger(L, -1)) {
          out += std::to_string(lua_tointeger(L, -1));
        } else if (double const d{ lua_tonumber(L, -1) }; std::isfinite(d)) {
          append_float(out, d);
        } else if (style.toml) {
          out += std::isnan(d) ? "nan" : d < 0 ? "-inf" : "inf";
        } else {
          codec_fail(style, "JSON has no inf or nan", path());
        }
        break;
      case LUA_TSTRING: {
        std::size_t len;
        char const *const s{ lua_tolstring(L, -1, &len) };
        append_string(out, { s, len }, style, path);
        break;
      }
      case LUA_TTABLE: {
        lua_pushvalue(L, -1);
        lua_rawget(L, visiting);
        bool const cycle{ lua_toboolean(L, -1) != 0 };
        lua_pop(L, 1);
        if (cycle) { codec_fail(style, "table contains itself", path()); }
        codec_table t{ codec_classify(L, -1, style, path) };
        if (t.array ? t.n == 0 : t.keys.empty()) {
          out += t.array ? "[]" : "{}";
          break;
        }
        if (!lua_checkstack(L, 4)) { codec_fail(style, "nesting too deep", path()); }
        lua_pushvalue(L, -1);
        lua_pushboolean(L, 1);
        lua_rawset(L, visiting);
        out += t.array ? "[" : style.toml ? "{ " : "{";
        frames.push_back({ lua_gettop(L), std::move(t), 0 });
        return;
      }
      default:
        codec_fail(style,
                   std::string{ "cannot encode a " } + lua_typename(L, lua_type(L, -1)),
                   path());
    }
    lua_pop(L, 1);
  } };

  lua_pushvalue(L, idx);
  for (;;) {
    write_top();
    for (;;) {  // close finished containers, then fetch the next element
      if (frames.empty()) { return; }
      frame &f{ frames.back() };
      if (f.next == (f.t.array ? static_cast<std::size_t>(f.t.n) : f.t.keys.size())) {
        newline(frames.size() - 1);
        out += f.t.array ? "]" : style.toml ? " }" : "}";
        lua_pushnil(L);
        lua_rawset(L, visiting);  // pops the container too: it was the key
        frames.pop_back();
        continue;
      }
      if (f.next) { out += style.toml ? ", " : ","; }
      newline(frames.size());
      if (f.t.array) {
        lua_rawgeti(L, f.idx, static_cast<lua_Integer>(++f.next));
      } else {
        std::string const &k{ f.t.keys[f.next++] };
        codec_append_key(out, k, style, path);
        out += style.toml ? " = " : style.indent ? ": " : ":";
        lua_pushlstring(L, k.data(), k.size());
        lua_rawget(L, f.idx);
      }
      break;
    }
  }
}

}  // namespace envy
