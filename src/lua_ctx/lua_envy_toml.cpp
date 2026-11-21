#include "lua_envy_toml.h"

#include "lua_envy_codec_util.h"
#include "lua_phase_context.h"
#include "sol_util.h"
#include "util.h"

#include "tomlc17.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace envy {
namespace {

void push_datetime(lua_State *L, toml_datum_t const &d) {
  auto const &t{ d.u.ts };
  char buf[48];
  int n{ 0 };
  if (d.type != TOML_TIME) {
    n += std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", t.year, t.month, t.day);
  }
  if (d.type == TOML_DATETIME || d.type == TOML_DATETIMETZ) { buf[n++] = 'T'; }
  if (d.type != TOML_DATE) {
    n += std::snprintf(buf + n,
                       sizeof buf - n,
                       "%02d:%02d:%02d",
                       t.hour,
                       t.minute,
                       t.second);
    if (t.usec) {
      n += std::snprintf(buf + n, sizeof buf - n, ".%06d", static_cast<int>(t.usec));
      while (buf[n - 1] == '0') { --n; }  // ".500000" -> ".5"
    }
  }
  if (d.type == TOML_DATETIMETZ) {
    int const a{ std::abs(t.tz) };
    n += t.tz ? std::snprintf(buf + n,
                              sizeof buf - n,
                              "%c%02d:%02d",
                              t.tz < 0 ? '-' : '+',
                              a / 60,
                              a % 60)
              : std::snprintf(buf + n, sizeof buf - n, "Z");
  }
  lua_pushlstring(L, buf, static_cast<std::size_t>(n));
}

// Pushes a scalar, or an empty table for a container and returns true to descend into it.
// `mt` indexes the array metatable, which tags every array.
bool push_node(lua_State *L, int mt, toml_datum_t const &d) {
  if (!lua_checkstack(L, 4)) {
    throw_runtime_error({ "envy.toml.decode: nesting too deep" });
  }
  switch (d.type) {
    case TOML_STRING:
      lua_pushlstring(L, d.u.str.ptr, static_cast<std::size_t>(d.u.str.len));
      return false;
    case TOML_INT64: lua_pushinteger(L, d.u.int64); return false;
    case TOML_FP64: lua_pushnumber(L, d.u.fp64); return false;
    case TOML_BOOLEAN: lua_pushboolean(L, d.u.boolean); return false;
    case TOML_DATE:
    case TOML_TIME:
    case TOML_DATETIME:
    case TOML_DATETIMETZ: push_datetime(L, d); return false;
    case TOML_ARRAY:
      lua_createtable(L, d.u.arr.size, 0);
      lua_pushvalue(L, mt);
      lua_setmetatable(L, -2);
      return true;
    case TOML_TABLE: lua_createtable(L, 0, d.u.tab.size); return true;
    case TOML_UNKNOWN: break;
  }
  lua_pushnil(L);  // a parsed tree never holds TOML_UNKNOWN
  return false;
}

// Walks the tree with an explicit frame stack: open containers wait on the Lua stack,
// so document depth costs heap, never C stack.
void push_tree(lua_State *L, toml_datum_t const &root) {
  struct frame {
    toml_datum_t const *d;
    int next;  // children pushed so far; for arrays, also the 1-based slot of the last
  };
  codec_push_array_mt(L);
  int const mt{ lua_gettop(L) };
  std::vector<frame> frames;
  if (push_node(L, mt, root)) { frames.push_back({ &root, 0 }); }
  while (!frames.empty()) {
    frame &f{ frames.back() };
    bool const table{ f.d->type == TOML_TABLE };
    if (f.next == (table ? f.d->u.tab.size : f.d->u.arr.size)) {
      frames.pop_back();  // the finished container is now a value for its parent
    } else {
      int const i{ f.next++ };
      if (table) {
        lua_pushlstring(L, f.d->u.tab.key[i], static_cast<std::size_t>(f.d->u.tab.len[i]));
      }
      toml_datum_t const &child{ table ? f.d->u.tab.value[i] : f.d->u.arr.elem[i] };
      if (push_node(L, mt, child)) {
        frames.push_back({ &child, 0 });
        continue;
      }
    }
    if (frames.empty()) { break; }
    if (frames.back().d->type == TOML_TABLE) {
      lua_rawset(L, -3);
    } else {
      lua_rawseti(L, -2, frames.back().next);
    }
  }
  lua_remove(L, mt);
}

sol::object decode(sol::this_state L, std::string_view text, std::string_view fn) {
  [[maybe_unused]] static bool const utf8_checked{ [] {  // once, before any parse
    toml_option_t opt{ toml_default_option() };
    opt.check_utf8 = true;
    toml_set_option(opt);
    return true;
  }() };

  if (text.size() > INT_MAX) { throw_runtime_error({ fn, ": input too large" }); }
  struct doc_t {
    toml_result_t r;
    ~doc_t() { toml_free(r); }
  } const doc{ toml_parse(text.empty() ? "" : text.data(),  // empty file: no buffer at all
                          static_cast<int>(text.size())) };
  if (!doc.r.ok) { throw_runtime_error({ fn, ": ", doc.r.errmsg }); }
  push_tree(L, doc.r.toptab);
  return sol::stack::pop<sol::object>(L);
}

std::string child_path(std::string const &parent,
                       std::string_view key,
                       codec_style const &style) {
  std::string p{ parent };
  if (!p.empty()) { p += '.'; }
  codec_append_key(p, key, style, [&] { return parent; });
  return p;
}

// For the value on top of the stack: -1 to write it inline, 0 for a [table] section,
// n > 0 for an [[array]] of n tables.
lua_Integer section_kind(lua_State *L,
                         codec_style const &style,
                         std::string const &where) {
  if (lua_type(L, -1) != LUA_TTABLE) { return -1; }
  auto const at{ [&] { return where; } };
  codec_table const t{ codec_classify(L, -1, style, at) };
  if (!t.array) { return t.keys.empty() ? -1 : 0; }
  for (lua_Integer i{ 1 }; i <= t.n; ++i) {
    lua_rawgeti(L, -1, i);
    bool const table{ lua_type(L, -1) == LUA_TTABLE &&
                      !codec_classify(L, -1, style, at).array };
    lua_pop(L, 1);
    if (!table) { return -1; }
  }
  return t.n ? t.n : -1;
}

// Emits a table's own keys, then its subtables as [sections] and [[arrays]] of tables.
// Pending tables wait on the Lua stack, one task each, so depth costs heap, not C stack.
std::string encode(lua_State *L, int idx, char const *fn) {
  codec_style const style{ fn, true, 0 };
  if (lua_type(L, idx) != LUA_TTABLE) {
    codec_fail(style, "a TOML document must be a table", {});
  }
  struct task {
    std::string path;  // header, in TOML key syntax; empty for the document itself
    bool element;      // one table of an array of tables: [[path]]
    bool exit;         // the table on top is done: clear its cycle mark and pop it
  };
  lua_newtable(L);
  int const visiting{ lua_gettop(L) };
  lua_pushvalue(L, idx);
  std::vector<task> tasks{ { {}, false, false } };
  std::string out;
  while (!tasks.empty()) {
    task const t{ std::move(tasks.back()) };
    tasks.pop_back();
    if (t.exit) {
      lua_pushnil(L);
      lua_rawset(L, visiting);  // pops the table too: it was the key
      continue;
    }

    int const self{ lua_gettop(L) };
    lua_pushvalue(L, self);
    lua_rawget(L, visiting);
    bool const cycle{ lua_toboolean(L, -1) != 0 };
    lua_pop(L, 1);
    if (cycle) { codec_fail(style, "table contains itself", t.path); }
    auto const at{ [&] { return t.path; } };
    codec_table const ct{ codec_classify(L, self, style, at) };
    if (ct.array) {
      codec_fail(style, "a TOML document must be a table, not an array", {});
    }
    lua_pushvalue(L, self);
    lua_pushboolean(L, 1);
    lua_rawset(L, visiting);

    struct section {
      std::string path;
      std::string const *key;
      lua_Integer tables;  // 0: [path]; n: [[path]] n times
    };
    std::vector<section> sections;
    std::string body;
    for (std::string const &k : ct.keys) {
      std::string path{ child_path(t.path, k, style) };
      lua_pushlstring(L, k.data(), k.size());
      lua_rawget(L, self);
      if (lua_Integer const kind{ section_kind(L, style, path) }; kind >= 0) {
        sections.push_back({ std::move(path), &k, kind });
      } else {
        codec_append_key(body, k, style, at);
        body += " = ";
        codec_encode(L, -1, visiting, style, path, body);
        body += '\n';
      }
      lua_pop(L, 1);
    }

    if (!t.path.empty() && (t.element || !body.empty())) {  // else implied by subtables
      if (!out.empty()) { out += '\n'; }
      out += t.element ? "[[" : "[";
      out += t.path;
      out += t.element ? "]]\n" : "]\n";
    }
    out += body;

    tasks.push_back({ {}, false, true });  // self stays on the stack under its subtables
    for (auto it{ sections.rbegin() }; it != sections.rend(); ++it) {
      if (!lua_checkstack(L, static_cast<int>(it->tables) + 4)) {
        codec_fail(style, "too many tables", it->path);
      }
      lua_pushlstring(L, it->key->data(), it->key->size());
      lua_rawget(L, self);
      if (!it->tables) {
        tasks.push_back({ std::move(it->path), false, false });
        continue;
      }
      int const array{ lua_gettop(L) };
      for (lua_Integer i{ it->tables }; i >= 1; --i) {
        lua_rawgeti(L, array, i);
        tasks.push_back({ it->path, true, false });
      }
      lua_remove(L, array);
    }
  }
  lua_pop(L, 1);  // visiting
  return out;
}

sol::object toml_decode(sol::this_state L, sol::stack_object text) {
  return decode(L, sol_util_string_arg(text, "envy.toml.decode"), "envy.toml.decode");
}

sol::object toml_read(sol::this_state L, std::string_view path) {
  std::vector<unsigned char> const bytes{
    lua_phase_context_read_file(L, path, "envy.toml.read")
  };
  return decode(L,
                { reinterpret_cast<char const *>(bytes.data()), bytes.size() },
                std::string{ "envy.toml.read: " } + std::string{ path });
}

std::string toml_encode(sol::this_state L, sol::stack_object value) {
  return encode(L, value.stack_index(), "envy.toml.encode");
}

void toml_write(sol::this_state L, std::string_view path, sol::stack_object value) {
  std::string const text{ encode(L, value.stack_index(), "envy.toml.write") };
  lua_phase_context_write_file(L, path, text, "envy.toml.write");
}

}  // namespace

void lua_envy_toml_install(sol::table &envy_table) {
  // set_function, not assignment: sol pushes an assigned function pointer without the
  // trampoline that turns C++ exceptions into Lua errors, so pcall could not catch them.
  sol::table toml{ envy_table.lua_state(), sol::create };
  toml.set_function("decode", toml_decode);
  toml.set_function("encode", toml_encode);
  toml.set_function("read", toml_read);
  toml.set_function("write", toml_write);
  toml.set_function("array", codec_array);
  envy_table["toml"] = toml;
}

}  // namespace envy
