#include "lua_envy_json.h"

#include "lua_envy_codec_util.h"
#include "lua_phase_context.h"
#include "sol_util.h"
#include "util.h"

#include "picojson.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace envy {
namespace {

// Iterative RFC 8259 reader on picojson's string lexer. Open containers wait on the Lua
// stack with a frame each in frames_, so nesting depth costs heap, never C stack.
class json_reader {
 public:
  json_reader(lua_State *L, std::string_view text)
      : L_{ L }, in_{ text.data(), text.data() + text.size() } {
    codec_push_array_mt(L_);
    mt_ = lua_gettop(L_);
  }

  // Leaves the document on the Lua stack; false on malformed or over-deep input.
  bool read() {
    for (;;) {
      switch (begin_value()) {
        case step::FAIL: return false;
        case step::OPENED: continue;  // read the new container's first element
        case step::DONE: break;
      }
      for (;;) {  // store the finished value into its container; close any that end here
        if (frames_.empty()) {
          lua_remove(L_, mt_);
          return true;
        }
        frame &f{ frames_.back() };
        if (f.object) {
          lua_rawset(L_, -3);
        } else {
          lua_rawseti(L_, -2, f.next++);
        }
        if (in_.expect(',')) {
          if (f.object && !key()) { return false; }
          break;
        }
        if (!in_.expect(f.object ? '}' : ']')) { return false; }
        frames_.pop_back();
      }
    }
  }

  bool at_end() {
    in_.skip_ws();
    return in_.getc() == -1;
  }
  int line() const { return in_.line(); }
  bool too_deep() const { return too_deep_; }

 private:
  enum class step { FAIL, OPENED, DONE };
  struct frame {
    bool object;
    lua_Integer next;  // arrays: next 1-based slot
  };

  step begin_value() {
    in_.skip_ws();
    switch (int const ch{ in_.getc() }) {
      case '{': return open(true);
      case '[': return open(false);
      case '"':
        str_.clear();
        if (!picojson::_parse_string(str_, in_)) { return step::FAIL; }
        lua_pushlstring(L_, str_.data(), str_.size());
        return step::DONE;
      case 'n':
      case 't':
      case 'f': return literal(ch);
      default: return number(ch) ? step::DONE : step::FAIL;
    }
  }

  step open(bool object) {
    if (!lua_checkstack(L_, 4)) {  // Lua's stack, not C's, bounds nesting
      too_deep_ = true;
      return step::FAIL;
    }
    lua_createtable(L_, 0, 0);
    if (!object) {
      lua_pushvalue(L_, mt_);
      lua_setmetatable(L_, -2);
    }
    if (in_.expect(object ? '}' : ']')) { return step::DONE; }
    frames_.push_back({ object, 1 });
    return object && !key() ? step::FAIL : step::OPENED;
  }

  bool key() {
    str_.clear();
    if (!in_.expect('"') || !picojson::_parse_string(str_, in_) || !in_.expect(':')) {
      return false;
    }
    lua_pushlstring(L_, str_.data(), str_.size());
    return true;
  }

  step literal(int ch) {  // ch already consumed: match the rest of null/true/false
    if (!in_.match(ch == 'n' ? "ull" : ch == 't' ? "rue" : "alse")) { return step::FAIL; }
    if (ch == 'n') {
      lua_pushnil(L_);
    } else {
      lua_pushboolean(L_, ch == 't');
    }
    return step::DONE;
  }

  // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?, checked as it is scanned. Integers
  // that fit stay Lua integers; the rest become floats.
  bool number(int ch) {
    str_.clear();
    auto const take{ [&] {
      str_ += static_cast<char>(ch);
      ch = in_.getc();
    } };
    auto const digits{ [&] {  // one or more
      if (ch < '0' || ch > '9') { return false; }
      while (ch >= '0' && ch <= '9') { take(); }
      return true;
    } };
    if (ch == '-') { take(); }
    if (ch == '0') {
      take();
    } else if (!digits()) {
      return false;
    }
    bool const frac{ ch == '.' };
    if (frac) {
      take();
      if (!digits()) { return false; }
    }
    bool const exp{ ch == 'e' || ch == 'E' };
    if (exp) {
      take();
      if (ch == '+' || ch == '-') { take(); }
      if (!digits()) { return false; }
    }
    in_.ungetc();  // the first character past the number

    char const *const end{ str_.c_str() + str_.size() };
    char *stop;
    if (!frac && !exp) {
      errno = 0;
      long long const i{ std::strtoll(str_.c_str(), &stop, 10) };
      if (errno == 0 && stop == end) {
        lua_pushinteger(L_, i);
        return true;
      }
    }
    double const d{ std::strtod(str_.c_str(), &stop) };
    if (stop != end) { return false; }
    lua_pushnumber(L_, d);
    return true;
  }

  lua_State *L_;
  picojson::input<char const *> in_;
  int mt_;  // stack slot of the array metatable
  std::vector<frame> frames_;
  std::string str_;  // reused across strings, keys and numbers
  bool too_deep_{ false };
};

sol::object decode(sol::this_state L, std::string_view text, std::string_view fn) {
  if (text.substr(0, 3) == "\xEF\xBB\xBF") { text.remove_prefix(3); }  // RFC 8259 §8.1
  if (!codec_utf8_valid(text)) { throw_runtime_error({ fn, ": invalid UTF-8" }); }
  json_reader r{ L, text };
  if (!r.read()) {
    if (r.too_deep()) { throw_runtime_error({ fn, ": nesting too deep" }); }
    throw_runtime_error({ fn, ": syntax error at line ", r.line() });
  }
  if (!r.at_end()) {
    throw_runtime_error({ fn, ": unexpected trailing characters at line ", r.line() });
  }
  return sol::stack::pop<sol::object>(L);
}

int indent_option(sol::optional<sol::table> const &opts, char const *fn) {
  if (!opts) { return 0; }
  for (auto const &[key, value] : *opts) {
    if (key.get_type() != sol::type::string || key.as<std::string>() != "indent") {
      throw_runtime_error({ fn, ": unknown option; only indent is supported" });
    }
  }
  sol::object const v{ (*opts)["indent"] };
  if (v.get_type() == sol::type::lua_nil) { return 0; }
  double const d{ v.get_type() == sol::type::number ? v.as<double>() : -1.0 };
  if (d < 0 || d > 10 || std::trunc(d) != d) {
    throw_runtime_error({ fn, ": indent must be an integer from 0 to 10" });
  }
  return static_cast<int>(d);
}

std::string encode(lua_State *L,
                   int idx,
                   sol::optional<sol::table> const &opts,
                   char const *fn) {
  codec_style const style{ fn, false, indent_option(opts, fn) };
  lua_newtable(L);
  int const visiting{ lua_gettop(L) };
  std::string out;
  codec_encode(L, idx, visiting, style, {}, out);
  lua_pop(L, 1);
  return out;
}

sol::object json_decode(sol::this_state L, sol::stack_object text) {
  return decode(L, sol_util_string_arg(text, "envy.json.decode"), "envy.json.decode");
}

sol::object json_read(sol::this_state L, std::string_view path) {
  std::vector<unsigned char> const bytes{
    lua_phase_context_read_file(L, path, "envy.json.read")
  };
  return decode(L,
                { reinterpret_cast<char const *>(bytes.data()), bytes.size() },
                std::string{ "envy.json.read: " } + std::string{ path });
}

std::string json_encode(sol::this_state L,
                        sol::stack_object value,
                        sol::optional<sol::table> opts) {
  return encode(L, value.stack_index(), opts, "envy.json.encode");
}

void json_write(sol::this_state L,
                std::string_view path,
                sol::stack_object value,
                sol::optional<sol::table> opts) {
  std::string const text{ encode(L, value.stack_index(), opts, "envy.json.write") + '\n' };
  lua_phase_context_write_file(L, path, text, "envy.json.write");
}

}  // namespace

void lua_envy_json_install(sol::table &envy_table) {
  // set_function, not assignment: sol pushes an assigned function pointer without the
  // trampoline that turns C++ exceptions into Lua errors, so pcall could not catch them.
  sol::table json{ envy_table.lua_state(), sol::create };
  json.set_function("decode", json_decode);
  json.set_function("encode", json_encode);
  json.set_function("read", json_read);
  json.set_function("write", json_write);
  json.set_function("array", codec_array);
  envy_table["json"] = json;
}

}  // namespace envy
