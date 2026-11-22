// Build-time Lua compiler for envy's own scripts: envy_luac <name> <in.lua> <out.luac>.
// Linked against envy's Lua, so the bytecode matches what the binary loads; debug info is
// kept, so tracebacks name the chunk `name` and its lines. A syntax error fails the build.

#include "lua.hpp"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

// lua_dump's writer: appends each piece to the std::string in `out`.
int append(lua_State *, void const *piece, size_t size, void *out) noexcept {
  try {
    static_cast<std::string *>(out)->append(static_cast<char const *>(piece), size);
    return 0;
  } catch (...) { return 1; }
}

std::optional<std::string> read_file(char const *path) {
  std::unique_ptr<std::FILE, decltype(&std::fclose)> const f{ std::fopen(path, "rb"),
                                                               &std::fclose };
  if (!f) { return std::nullopt; }
  std::string out;
  char buf[4096];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f.get())) > 0;) { out.append(buf, n); }
  return std::ferror(f.get()) ? std::nullopt : std::optional{ std::move(out) };
}

}  // namespace

int main(int argc, char **argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: envy_luac <name> <in.lua> <out.luac>\n");
    return 2;
  }
  auto const source{ read_file(argv[2]) };
  if (!source) {
    std::fprintf(stderr, "envy_luac: cannot read %s\n", argv[2]);
    return 1;
  }
  std::string const chunk_name{ std::string{ "=" } + argv[1] };

  std::unique_ptr<lua_State, decltype(&lua_close)> const L{ luaL_newstate(), &lua_close };
  if (!L) { return 1; }
  if (luaL_loadbufferx(L.get(), source->data(), source->size(), chunk_name.c_str(), "t") !=
      LUA_OK) {
    std::fprintf(stderr, "envy_luac: %s: %s\n", argv[2], lua_tostring(L.get(), -1));
    return 1;
  }
  std::string bytecode;
  if (lua_dump(L.get(), append, &bytecode, 0) != 0) {
    std::fprintf(stderr, "envy_luac: %s: dump failed\n", argv[2]);
    return 1;
  }

  std::unique_ptr<std::FILE, decltype(&std::fclose)> const out{ std::fopen(argv[3], "wb"),
                                                                 &std::fclose };
  if (!out || std::fwrite(bytecode.data(), 1, bytecode.size(), out.get()) != bytecode.size()) {
    std::fprintf(stderr, "envy_luac: cannot write %s\n", argv[3]);
    return 1;
  }
  return 0;
}
