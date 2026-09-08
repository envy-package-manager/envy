#include "shell_hooks.h"

#include "doctest.h"
#include "version.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>

namespace {

struct temp_dir_fixture {
  temp_dir_fixture() {
    static std::mt19937_64 rng{ std::random_device{}() };
    root = std::filesystem::temp_directory_path() /
           ("envy-shell-hooks-test-" + std::to_string(rng()));
    std::filesystem::create_directories(root);
  }

  ~temp_dir_fixture() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  void write_file(std::filesystem::path const &p, std::string_view content) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out{ p, std::ios::binary };
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
  }

  std::string read_file(std::filesystem::path const &p) {
    std::ifstream in{ p, std::ios::binary };
    return { std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
  }

  std::string hook_text(char const *ext) {
    return read_file(root / "shell" / ("hook." + std::string{ ext }));
  }

  // hook.<ext>'s stamp token, <envy version>:<resource digest>; empty when absent.
  std::string stamp(char const *ext) {
    std::string const content{ hook_text(ext) };
    auto const key{ content.find("_ENVY_HOOK_STAMP") };
    if (key == std::string::npos) { return {}; }
    auto const begin{ content.find_first_not_of("= \"", key + 16) };
    auto const end{ content.find_first_of("\"\r\n \t", begin) };
    return content.substr(begin, end - begin);
  }

  std::string writer(char const *ext) {
    std::string const s{ stamp(ext) };
    return s.substr(0, s.find(':'));
  }

  std::string digest(char const *ext) {
    std::string const s{ stamp(ext) };
    return s.substr(s.find(':') + 1);
  }

  // Rewrite hook.<ext>'s stamp: stands in for a file some other envy build left behind.
  void restamp(char const *ext, std::string const &writer, std::string const &digest) {
    auto const p{ root / "shell" / ("hook." + std::string{ ext }) };
    std::string text{ read_file(p) };
    auto const key{ text.find("_ENVY_HOOK_STAMP") };
    auto const begin{ text.find_first_not_of("= \"", key + 16) };
    auto const end{ text.find_first_of("\"\r\n \t", begin) };
    text.replace(begin, end - begin, writer + ":" + digest);
    write_file(p, text);
  }

  std::filesystem::path root;
};

constexpr char const *kExts[] = { "bash", "zsh", "fish", "ps1" };

}  // namespace

TEST_CASE_FIXTURE(temp_dir_fixture, "shell_hooks: ensure") {
  namespace fs = std::filesystem;
  using envy::shell_hooks::ensure;

  SUBCASE("creates all 4 hook files in empty cache") {
    CHECK(ensure(root) == 4);
    for (auto const *ext : kExts) {
      CHECK(fs::exists(root / "shell" / ("hook." + std::string{ ext })));
    }
  }

  SUBCASE("second ensure writes nothing") {
    ensure(root);
    CHECK(ensure(root) == 0);
  }

  SUBCASE("an untouched hook keeps its mtime") {
    ensure(root);
    auto const fish_hook{ root / "shell" / "hook.fish" };
    auto const before{ fs::last_write_time(fish_hook) };
    ensure(root);
    CHECK(before == fs::last_write_time(fish_hook));
  }

  // The regression #335 shipped into a void: a hook edit with the stamp left alone.
  SUBCASE("a hook whose body drifts from the binary's is rewritten") {
    ensure(root);
    auto const zsh_hook{ root / "shell" / "hook.zsh" };
    write_file(zsh_hook, read_file(zsh_hook) + "# drifted\n");  // stamp intact, body stale
    CHECK(ensure(root) == 1);
    CHECK(hook_text("zsh").find("# drifted") == std::string::npos);
  }

  // Hooks are shared at <root>/shell/ while binaries sit in <root>/envy/<version>/, so a
  // version-pinned project and a newer envy meet here. Neither may fight over the file.
  SUBCASE("a hook a newer envy wrote is left alone") {
    ensure(root);
    restamp("zsh", "999.0.0", "0123456789ab");
    CHECK(ensure(root) == 0);
    CHECK(writer("zsh") == "999.0.0");
  }

  SUBCASE("an identical hook from another envy is not relabeled") {
    ensure(root);
    restamp("zsh", "1.2.3", digest("zsh"));
    CHECK(ensure(root) == 0);
    CHECK(writer("zsh") == "1.2.3");
  }

  SUBCASE("a hook stamped by the retired integer scheme is rewritten") {
    ensure(root);
    write_file(root / "shell" / "hook.bash",
               "# old\n_ENVY_HOOK_VERSION=99999\n# stale content\n");
    CHECK(ensure(root) == 1);
    CHECK(hook_text("bash").find("stale content") == std::string::npos);
  }

  SUBCASE("an empty hook is rewritten") {
    ensure(root);
    write_file(root / "shell" / "hook.ps1", "");
    CHECK(ensure(root) == 1);
    CHECK(!hook_text("ps1").empty());
  }

  SUBCASE("a missing hook is recreated without touching the others") {
    ensure(root);
    fs::remove(root / "shell" / "hook.fish");
    CHECK(ensure(root) == 1);
    CHECK(fs::exists(root / "shell" / "hook.fish"));
  }

  SUBCASE("written hooks carry writer and digest, not placeholders") {
    ensure(root);
    for (auto const *ext : kExts) {
      for (auto const *token : { "@@ENVY_RESOURCE_HASH@@", "@@ENVY_HOOK_WRITER@@" }) {
        CHECK_MESSAGE(hook_text(ext).find(token) == std::string::npos,
                      "placeholder not replaced in hook.",
                      ext);
      }
      // A writer the tie-break cannot parse would freeze every hook on this machine.
      CHECK_MESSAGE(envy::version_is_newer("999.0.0", writer(ext)),
                    "unparseable writer version in hook.",
                    ext);
      CHECK_MESSAGE(digest(ext).size() == 12, "no content digest in hook.", ext);
    }
  }

  // Per-resource digests: one hash over all four would refresh every hook on any edit.
  SUBCASE("each hook's digest is its own") {
    ensure(root);
    std::set<std::string> digests;
    for (auto const *ext : kExts) { digests.insert(digest(ext)); }
    CHECK(digests.size() == 4);
  }

  SUBCASE("written hooks contain managed-by comment") {
    ensure(root);
    for (auto const *ext : kExts) {
      CHECK_MESSAGE(hook_text(ext).find("managed by envy") != std::string::npos,
                    "missing managed-by comment in hook.",
                    ext);
    }
  }
}
