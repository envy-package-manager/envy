#include "util.h"

#include "platform.h"

#include "doctest.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

namespace {

std::filesystem::path make_temp_path(char const *tag) {
  static std::atomic<int> counter{ 0 };
  auto const id = counter.fetch_add(1, std::memory_order_relaxed);
  auto base{ std::filesystem::temp_directory_path() };
  return base / ("envy-util-test-" + std::string(tag) + "-" + std::to_string(id));
}

void write_dummy_file(std::filesystem::path const &path) {
  std::ofstream out{ path };
  out << "envy-test";
}

}  // namespace

TEST_CASE("match with std::variant of int and string") {
  using var_t = std::variant<int, std::string>;

  var_t v1{ 42 };
  var_t v2{ std::string("hello") };

  auto result1{ std::visit(
      envy::match{ [](int x) { return x * 2; },
                   [](std::string const &s) { return static_cast<int>(s.size()); } },
      v1) };

  auto result2{ std::visit(
      envy::match{ [](int x) { return x * 2; },
                   [](std::string const &s) { return static_cast<int>(s.size()); } },
      v2) };

  CHECK(result1 == 84);
  CHECK(result2 == 5);
}

TEST_CASE("match with different return types") {
  using var_t = std::variant<int, double>;

  var_t v1{ 42 };
  var_t v2{ 3.14 };

  auto result1{ std::visit(envy::match{ [](int x) { return std::to_string(x); },
                                        [](double d) { return std::to_string(d); } },
                           v1) };

  auto result2{ std::visit(envy::match{ [](int x) { return std::to_string(x); },
                                        [](double d) { return std::to_string(d); } },
                           v2) };

  CHECK(result1 == "42");
  CHECK(result2 == "3.140000");
}

TEST_CASE("match with three alternatives") {
  using var_t = std::variant<int, double, std::string>;

  var_t v1{ 42 };
  var_t v2{ 3.14 };
  var_t v3{ std::string("test") };

  auto visitor{ envy::match{ [](int x) { return 1; },
                             [](double) { return 2; },
                             [](std::string const &) { return 3; } } };

  CHECK(std::visit(visitor, v1) == 1);
  CHECK(std::visit(visitor, v2) == 2);
  CHECK(std::visit(visitor, v3) == 3);
}

TEST_CASE("match with capturing lambdas") {
  using var_t = std::variant<int, double>;

  int multiplier{ 10 };
  double divisor{ 2.0 };

  var_t v1{ 5 };
  var_t v2{ 10.0 };

  auto visitor{ envy::match{ [&](int x) { return x * multiplier; },
                             [&](double d) { return static_cast<int>(d / divisor); } } };

  CHECK(std::visit(visitor, v1) == 50);
  CHECK(std::visit(visitor, v2) == 5);
}

TEST_CASE("match with void return") {
  using var_t = std::variant<int, std::string>;

  int int_count{};
  int string_count{};

  var_t v1{ 42 };
  var_t v2{ std::string("test") };

  auto counter{ envy::match{ [&](int) { ++int_count; },
                             [&](std::string const &) { ++string_count; } } };

  std::visit(counter, v1);
  std::visit(counter, v2);
  std::visit(counter, v1);

  CHECK(int_count == 2);
  CHECK(string_count == 1);
}

// Hex conversion tests

TEST_CASE("util_bytes_to_hex converts empty input") {
  std::string result = envy::util_bytes_to_hex("", 0);
  CHECK(result.empty());
}

TEST_CASE("util_bytes_to_hex converts single byte") {
  unsigned char data[] = { 0xab };
  std::string result = envy::util_bytes_to_hex(data, 1);
  CHECK(result == "ab");
}

TEST_CASE("util_bytes_to_hex converts multiple bytes") {
  unsigned char data[] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
  std::string result = envy::util_bytes_to_hex(data, 8);
  CHECK(result == "0123456789abcdef");
}

TEST_CASE("util_bytes_to_hex produces lowercase") {
  unsigned char data[] = { 0xff, 0xaa, 0xbb, 0xcc };
  std::string result = envy::util_bytes_to_hex(data, 4);
  CHECK(result == "ffaabbcc");
}

TEST_CASE("util_bytes_to_hex handles zero bytes") {
  unsigned char data[] = { 0x00, 0x00, 0x00 };
  std::string result = envy::util_bytes_to_hex(data, 3);
  CHECK(result == "000000");
}

TEST_CASE("util_bytes_to_hex handles all byte values") {
  unsigned char data[256];
  for (int i = 0; i < 256; ++i) { data[i] = static_cast<unsigned char>(i); }
  std::string result = envy::util_bytes_to_hex(data, 256);
  CHECK(result.size() == 512);
  // Spot check a few values
  CHECK(result.substr(0, 2) == "00");
  CHECK(result.substr(2, 2) == "01");
  CHECK(result.substr(254, 2) == "7f");
  CHECK(result.substr(510, 2) == "ff");
}

TEST_CASE("util_hex_to_bytes converts empty string") {
  auto result = envy::util_hex_to_bytes("");
  CHECK(result.empty());
}

TEST_CASE("util_hex_to_bytes converts single byte lowercase") {
  auto result = envy::util_hex_to_bytes("ab");
  REQUIRE(result.size() == 1);
  CHECK(result[0] == 0xab);
}

TEST_CASE("util_hex_to_bytes converts single byte uppercase") {
  auto result = envy::util_hex_to_bytes("AB");
  REQUIRE(result.size() == 1);
  CHECK(result[0] == 0xab);
}

TEST_CASE("util_hex_to_bytes converts single byte mixed case") {
  auto result = envy::util_hex_to_bytes("Ab");
  REQUIRE(result.size() == 1);
  CHECK(result[0] == 0xab);
}

TEST_CASE("util_hex_to_bytes converts multiple bytes") {
  auto result = envy::util_hex_to_bytes("0123456789abcdef");
  REQUIRE(result.size() == 8);
  CHECK(result[0] == 0x01);
  CHECK(result[1] == 0x23);
  CHECK(result[2] == 0x45);
  CHECK(result[3] == 0x67);
  CHECK(result[4] == 0x89);
  CHECK(result[5] == 0xab);
  CHECK(result[6] == 0xcd);
  CHECK(result[7] == 0xef);
}

TEST_CASE("util_hex_to_bytes handles zero bytes") {
  auto result = envy::util_hex_to_bytes("000000");
  REQUIRE(result.size() == 3);
  CHECK(result[0] == 0x00);
  CHECK(result[1] == 0x00);
  CHECK(result[2] == 0x00);
}

TEST_CASE("util_hex_to_bytes handles all uppercase") {
  auto result = envy::util_hex_to_bytes("FFAABBCC");
  REQUIRE(result.size() == 4);
  CHECK(result[0] == 0xff);
  CHECK(result[1] == 0xaa);
  CHECK(result[2] == 0xbb);
  CHECK(result[3] == 0xcc);
}

TEST_CASE("util_hex_to_bytes throws on odd length") {
  CHECK_THROWS_WITH(envy::util_hex_to_bytes("a"),
                    "util_hex_to_bytes: hex string must have even length, got 1");
  CHECK_THROWS_WITH(envy::util_hex_to_bytes("abc"),
                    "util_hex_to_bytes: hex string must have even length, got 3");
}

TEST_CASE("util_hex_to_bytes throws on invalid character") {
  CHECK_THROWS_WITH(envy::util_hex_to_bytes("ag"),
                    "util_hex_to_bytes: invalid character at position 1");
  CHECK_THROWS_WITH(envy::util_hex_to_bytes("0z"),
                    "util_hex_to_bytes: invalid character at position 1");
  CHECK_THROWS_WITH(envy::util_hex_to_bytes("!0"),
                    "util_hex_to_bytes: invalid character at position 0");
  CHECK_THROWS_WITH(envy::util_hex_to_bytes(" 0"),
                    "util_hex_to_bytes: invalid character at position 0");
}

TEST_CASE("util_bytes_to_hex and util_hex_to_bytes round-trip") {
  unsigned char original[] = { 0x00, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xff };
  std::string hex = envy::util_bytes_to_hex(original, 9);
  auto recovered = envy::util_hex_to_bytes(hex);
  REQUIRE(recovered.size() == 9);
  CHECK(std::memcmp(original, recovered.data(), 9) == 0);
}

TEST_CASE("util_hex_to_bytes and util_bytes_to_hex round-trip") {
  std::string original_hex = "0123456789abcdefABCDEF";
  auto bytes = envy::util_hex_to_bytes(original_hex);
  std::string recovered_hex = envy::util_bytes_to_hex(bytes.data(), bytes.size());
  // Result should be lowercase
  CHECK(recovered_hex == "0123456789abcdefabcdef");
}

TEST_CASE("util_format_bytes uses integer for bytes") {
  CHECK(envy::util_format_bytes(0) == "0B");
  CHECK(envy::util_format_bytes(1) == "1B");
  CHECK(envy::util_format_bytes(1023) == "1023B");
}

TEST_CASE("util_format_bytes scales to KB with one decimal") {
  CHECK(envy::util_format_bytes(1024) == "1.00KB");
  CHECK(envy::util_format_bytes(1536) == "1.50KB");
  CHECK(envy::util_format_bytes(10 * 1024) == "10.00KB");
}

TEST_CASE("util_format_bytes scales to MB/GB/TB") {
  constexpr std::uint64_t kMB{ 1024ull * 1024ull };
  constexpr std::uint64_t kGB{ kMB * 1024ull };
  constexpr std::uint64_t kTB{ kGB * 1024ull };

  CHECK(envy::util_format_bytes(kMB) == "1.00MB");
  CHECK(envy::util_format_bytes(static_cast<std::uint64_t>(1.75 * kMB)) == "1.75MB");
  CHECK(envy::util_format_bytes(5 * kGB) == "5.00GB");
  CHECK(envy::util_format_bytes(3 * kTB) == "3.00TB");
}

TEST_CASE("scoped_path_cleanup removes file on destruction") {
  auto path = make_temp_path("cleanup");
  write_dummy_file(path);
  REQUIRE(std::filesystem::exists(path));
  {
    envy::scoped_path_cleanup cleanup{ path };
    CHECK(std::filesystem::exists(path));
  }
  CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("scoped_path_cleanup reset switches targets and cleans previous file") {
  auto first = make_temp_path("first");
  auto second = make_temp_path("second");
  write_dummy_file(first);
  write_dummy_file(second);
  REQUIRE(std::filesystem::exists(first));
  REQUIRE(std::filesystem::exists(second));

  {
    envy::scoped_path_cleanup cleanup{ first };
    CHECK(std::filesystem::exists(first));
    cleanup.reset(second);
    CHECK_FALSE(std::filesystem::exists(first));
    CHECK(std::filesystem::exists(second));
  }

  CHECK_FALSE(std::filesystem::exists(second));
}

TEST_CASE("util_load_file loads empty file") {
  auto path = make_temp_path("empty");
  std::ignore = std::ofstream{ path };  // Create empty file
  envy::scoped_path_cleanup cleanup{ path };

  auto data = envy::util_load_file(path);
  CHECK(data.empty());
}

TEST_CASE("util_load_file loads small text file") {
  auto path = make_temp_path("small");
  {
    std::ofstream out{ path, std::ios::binary };
    out << "hello world";
  }
  envy::scoped_path_cleanup cleanup{ path };

  auto data = envy::util_load_file(path);
  REQUIRE(data.size() == 11);
  CHECK(std::memcmp(data.data(), "hello world", 11) == 0);
}

TEST_CASE("util_load_file loads binary data") {
  auto path = make_temp_path("binary");
  unsigned char const test_data[] = { 0x00, 0x01, 0x02, 0xff, 0xfe, 0xfd };
  {
    std::ofstream out{ path, std::ios::binary };
    out.write(reinterpret_cast<char const *>(test_data), sizeof(test_data));
  }
  envy::scoped_path_cleanup cleanup{ path };

  auto data = envy::util_load_file(path);
  REQUIRE(data.size() == 6);
  CHECK(data[0] == 0x00);
  CHECK(data[1] == 0x01);
  CHECK(data[2] == 0x02);
  CHECK(data[3] == 0xff);
  CHECK(data[4] == 0xfe);
  CHECK(data[5] == 0xfd);
}

TEST_CASE("util_load_file loads larger file") {
  auto path = make_temp_path("large");
  std::vector<unsigned char> test_data(10000);
  for (size_t i = 0; i < test_data.size(); ++i) {
    test_data[i] = static_cast<unsigned char>(i % 256);
  }
  {
    std::ofstream out{ path, std::ios::binary };
    out.write(reinterpret_cast<char const *>(test_data.data()), test_data.size());
  }
  envy::scoped_path_cleanup cleanup{ path };

  auto data = envy::util_load_file(path);
  REQUIRE(data.size() == 10000);
  CHECK(std::memcmp(data.data(), test_data.data(), 10000) == 0);
}

TEST_CASE("util_load_file throws on nonexistent file") {
  auto path = make_temp_path("nonexistent");
  CHECK_THROWS_WITH(envy::util_load_file(path), doctest::Contains("failed to open file"));
}

TEST_CASE("util_load_file handles files with null bytes") {
  auto path = make_temp_path("nullbytes");
  unsigned char const test_data[] = { 'a', 'b', 0x00, 'c', 'd', 0x00, 0x00, 'e' };
  {
    std::ofstream out{ path, std::ios::binary };
    out.write(reinterpret_cast<char const *>(test_data), sizeof(test_data));
  }
  envy::scoped_path_cleanup cleanup{ path };

  auto data = envy::util_load_file(path);
  REQUIRE(data.size() == 8);
  CHECK(data[0] == 'a');
  CHECK(data[1] == 'b');
  CHECK(data[2] == 0x00);
  CHECK(data[3] == 'c');
  CHECK(data[4] == 'd');
  CHECK(data[5] == 0x00);
  CHECK(data[6] == 0x00);
  CHECK(data[7] == 'e');
}

TEST_CASE("util_flatten_script_with_semicolons handles empty script") {
  CHECK(envy::util_flatten_script_with_semicolons("") == "");
}

TEST_CASE("util_flatten_script_with_semicolons handles single line") {
  CHECK(envy::util_flatten_script_with_semicolons("python script.py") ==
        "python script.py");
}

TEST_CASE("util_flatten_script_with_semicolons replaces newlines with semicolons") {
  CHECK(envy::util_flatten_script_with_semicolons("cmd1\ncmd2\ncmd3") ==
        "cmd1; cmd2; cmd3");
}

TEST_CASE("util_flatten_script_with_semicolons handles no trailing semicolon") {
  std::string const script{ "python build/gen.py\nninja -C out\nout/gn_unittests" };
  CHECK(envy::util_flatten_script_with_semicolons(script) ==
        "python build/gen.py; ninja -C out; out/gn_unittests");
}

TEST_CASE("util_flatten_script_with_semicolons handles carriage returns") {
  CHECK(envy::util_flatten_script_with_semicolons("cmd1\rcmd2") == "cmd1; cmd2");
}

TEST_CASE("util_flatten_script_with_semicolons handles windows line endings") {
  CHECK(envy::util_flatten_script_with_semicolons("cmd1\r\ncmd2\r\ncmd3") ==
        "cmd1; cmd2; cmd3");
}

TEST_CASE("util_flatten_script_with_semicolons collapses multiple spaces") {
  CHECK(envy::util_flatten_script_with_semicolons("cmd   arg1    arg2") ==
        "cmd arg1 arg2");
}

TEST_CASE("util_flatten_script_with_semicolons collapses tabs") {
  CHECK(envy::util_flatten_script_with_semicolons("cmd\t\targ1\targ2") == "cmd arg1 arg2");
}

TEST_CASE("util_flatten_script_with_semicolons trims leading whitespace per line") {
  CHECK(envy::util_flatten_script_with_semicolons("  cmd1\n  cmd2") == "cmd1; cmd2");
}

TEST_CASE("util_flatten_script_with_semicolons trims trailing whitespace") {
  CHECK(envy::util_flatten_script_with_semicolons("cmd1\ncmd2  \n") == "cmd1; cmd2");
}

TEST_CASE("util_flatten_script_with_semicolons handles empty lines") {
  CHECK(envy::util_flatten_script_with_semicolons("cmd1\n\ncmd2") == "cmd1; cmd2");
}

TEST_CASE("util_flatten_script_with_semicolons handles multiple empty lines") {
  CHECK(envy::util_flatten_script_with_semicolons("cmd1\n\n\ncmd2") == "cmd1; cmd2");
}

TEST_CASE("util_flatten_script_with_semicolons handles mixed whitespace") {
  std::string const script{ "  cmd1 arg1  \n\t cmd2  arg2\t\n  cmd3  " };
  CHECK(envy::util_flatten_script_with_semicolons(script) == "cmd1 arg1; cmd2 arg2; cmd3");
}

TEST_CASE("util_flatten_script_with_semicolons handles real ninja example") {
  std::string const script{
    "python build/gen.py\n"
    "ninja -C out\n"
    "out/gn_unittests"
  };
  CHECK(envy::util_flatten_script_with_semicolons(script) ==
        "python build/gen.py; ninja -C out; out/gn_unittests");
}

TEST_CASE("util_flatten_script_with_semicolons handles complex real-world script") {
  std::string const script{
    "python ./configure.py --bootstrap --gtest-source-dir=googletest\n"
    "./ninja all\n"
    "./ninja_test"
  };
  CHECK(envy::util_flatten_script_with_semicolons(script) ==
        "python ./configure.py --bootstrap --gtest-source-dir=googletest; ./ninja all; "
        "./ninja_test");
}

TEST_CASE("util_flatten_script_with_semicolons preserves internal semicolons") {
  CHECK(envy::util_flatten_script_with_semicolons("cmd1 ; cmd2\ncmd3") ==
        "cmd1 ; cmd2; cmd3");
}

TEST_CASE("util_simplify_cache_paths handles empty command") {
  std::filesystem::path const cache_root{ "/path/to/cache" };
  CHECK(envy::util_simplify_cache_paths("", cache_root) == "");
}

TEST_CASE("util_simplify_cache_paths handles empty cache root") {
  CHECK(envy::util_simplify_cache_paths("python script.py", std::filesystem::path{}) ==
        "python script.py");
}

TEST_CASE("util_simplify_cache_paths preserves command without cache paths") {
  std::filesystem::path const cache_root{ "/path/to/cache" };
  std::string const cmd{ "python script.py --arg value" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root) == cmd);
}

TEST_CASE("util_simplify_cache_paths replaces single cache path") {
  std::filesystem::path const cache_root{ "/home/user/.cache/envy" };
  std::string const cmd{ "/home/user/.cache/envy/assets/local.python@r0/bin/python" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root) == "python");
}

TEST_CASE("util_simplify_cache_paths replaces cache paths in command with args") {
  std::filesystem::path const cache_root{ "/cache" };
  std::string const cmd{ "/cache/assets/python/bin/python /cache/assets/script/run.py" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root) == "python run.py");
}

TEST_CASE("util_simplify_cache_paths preserves non-cache paths") {
  std::filesystem::path const cache_root{ "/cache" };
  std::string const cmd{ "/cache/assets/python/bin/python /usr/local/bin/script.sh" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root) ==
        "python /usr/local/bin/script.sh");
}

TEST_CASE("util_simplify_cache_paths handles mixed whitespace") {
  std::filesystem::path const cache_root{ "/cache" };
  std::string const cmd{ "/cache/bin/tool  \t arg1\n/cache/bin/other" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root) == "tool  \t arg1\nother");
}

TEST_CASE("util_simplify_cache_paths preserves leading/trailing whitespace") {
  std::filesystem::path const cache_root{ "/cache" };
  std::string const cmd{ "  /cache/bin/python script.py  " };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root) == "  python script.py  ");
}

TEST_CASE("util_simplify_cache_paths handles partial cache path match") {
  std::filesystem::path const cache_root{ "/home/cache" };
  std::string const cmd{ "/home/cacheother/bin/tool" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root) == "/home/cacheother/bin/tool");
}

TEST_CASE("util_simplify_cache_paths handles complex real-world example") {
  std::filesystem::path const cache_root{ "/Users/charlesnicholson/Library/Caches/envy" };
  std::string const cmd{
    "/Users/charlesnicholson/Library/Caches/envy/assets/local.python@r0/"
    "darwin-arm64-blake3-abc123/assets/installed/bin/python3 ./configure.py --bootstrap"
  };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root) ==
        "python3 ./configure.py --bootstrap");
}

// Product matching tests

TEST_CASE("util_simplify_cache_paths matches product by suffix") {
  std::filesystem::path const cache_root{ "/cache" };
  envy::product_map_t const products{ { "cmake", "bin/cmake" } };
  std::string const cmd{ "/cache/assets/cmake@v1/bin/cmake --version" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root, products) == "cmake --version");
}

TEST_CASE("util_simplify_cache_paths matches product with .exe suffix") {
  std::filesystem::path const cache_root{ "/cache" };
  envy::product_map_t const products{ { "cmake", "bin/cmake.exe" } };
  std::string const cmd{ "/cache/assets/cmake@v1/bin/cmake.exe --version" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root, products) == "cmake --version");
}

TEST_CASE("util_simplify_cache_paths matches multiple products") {
  std::filesystem::path const cache_root{ "/cache" };
  envy::product_map_t const products{ { "cmake", "bin/cmake.exe" },
                                      { "ninja", "bin/ninja.exe" } };
  std::string const cmd{
    "/cache/cmake@v1/bin/cmake.exe -G Ninja /cache/ninja@v1/bin/ninja.exe"
  };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root, products) ==
        "cmake -G Ninja ninja");
}

TEST_CASE("util_simplify_cache_paths product takes precedence over cache fallback") {
  std::filesystem::path const cache_root{ "/cache" };
  envy::product_map_t const products{ { "my-cmake", "bin/cmake.exe" } };
  std::string const cmd{ "/cache/cmake@v1/bin/cmake.exe" };
  // Product match should return "my-cmake", not filename "cmake.exe"
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root, products) == "my-cmake");
}

TEST_CASE("util_simplify_cache_paths falls back to filename when no product match") {
  std::filesystem::path const cache_root{ "/cache" };
  envy::product_map_t const products{ { "ninja", "bin/ninja.exe" } };
  std::string const cmd{ "/cache/cmake@v1/bin/cmake.exe" };
  // No product match for cmake, should fall back to filename extraction
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root, products) == "cmake.exe");
}

TEST_CASE("util_simplify_cache_paths handles Windows backslash paths with products") {
  std::filesystem::path const cache_root{ "C:\\Users\\test\\.cache\\envy" };
  envy::product_map_t const products{ { "cmake", "bin\\cmake.exe" } };
  std::string const cmd{
    "C:\\Users\\test\\.cache\\envy\\cmake@v1\\bin\\cmake.exe --version"
  };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root, products) == "cmake --version");
}

TEST_CASE("util_simplify_cache_paths handles mixed slash styles") {
  std::filesystem::path const cache_root{ "/cache" };
  envy::product_map_t const products{ { "cmake", "bin/cmake.exe" } };
  // Command uses backslashes but product uses forward slashes
  std::string const cmd{ "/cache/cmake@v1\\bin\\cmake.exe --version" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root, products) == "cmake --version");
}

TEST_CASE("util_simplify_cache_paths Windows cache_root with forward slash command") {
  std::filesystem::path const cache_root{ "C:\\cache" };
  // Command uses forward slashes (common in scripts)
  std::string const cmd{ "C:/cache/assets/python/bin/python.exe script.py" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root) == "python.exe script.py");
}

TEST_CASE("util_simplify_cache_paths product with nested path") {
  std::filesystem::path const cache_root{ "/cache" };
  envy::product_map_t const products{ { "arm-gcc",
                                        "arm-none-eabi/bin/arm-none-eabi-gcc" } };
  std::string const cmd{
    "/cache/toolchain@v1/arm-none-eabi/bin/arm-none-eabi-gcc -c foo.c"
  };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root, products) == "arm-gcc -c foo.c");
}

TEST_CASE("util_simplify_cache_paths empty products behaves like before") {
  std::filesystem::path const cache_root{ "/cache" };
  envy::product_map_t const products{};
  std::string const cmd{ "/cache/python@v1/bin/python3 script.py" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root, products) == "python3 script.py");
}

// key=value tests

TEST_CASE("util_simplify_cache_paths simplifies key=value RHS with cache path") {
  std::filesystem::path const cache_root{ "/cache" };
  std::string const cmd{ "python --gtest-dir=/cache/gtest@v1/lib/gtest" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root) == "python --gtest-dir=gtest");
}

TEST_CASE("util_simplify_cache_paths simplifies key=value RHS with product") {
  std::filesystem::path const cache_root{ "/cache" };
  envy::product_map_t const products{ { "googletest", "lib/googletest" } };
  std::string const cmd{ "python --gtest=/cache/gtest@v1/lib/googletest" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root, products) ==
        "python --gtest=googletest");
}

TEST_CASE("util_simplify_cache_paths preserves key=value when RHS is not cache path") {
  std::filesystem::path const cache_root{ "/cache" };
  std::string const cmd{ "cmake -DCMAKE_BUILD_TYPE=Release" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root) ==
        "cmake -DCMAKE_BUILD_TYPE=Release");
}

TEST_CASE("util_simplify_cache_paths handles multiple key=value pairs") {
  std::filesystem::path const cache_root{ "/cache" };
  envy::product_map_t const products{ { "ninja", "bin/ninja" }, { "cmake", "bin/cmake" } };
  std::string const cmd{
    "-DCMAKE_MAKE_PROGRAM=/cache/ninja@v1/bin/ninja "
    "-DCMAKE_C_COMPILER=/cache/gcc@v1/bin/gcc"
  };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root, products) ==
        "-DCMAKE_MAKE_PROGRAM=ninja -DCMAKE_C_COMPILER=gcc");
}

TEST_CASE("util_simplify_cache_paths handles Windows backslash in key=value") {
  std::filesystem::path const cache_root{ "C:\\cache" };
  envy::product_map_t const products{ { "ninja", "bin\\ninja.exe" } };
  std::string const cmd{ "-DCMAKE_MAKE_PROGRAM=C:\\cache\\ninja@v1\\bin\\ninja.exe" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root, products) ==
        "-DCMAKE_MAKE_PROGRAM=ninja");
}

TEST_CASE("util_simplify_cache_paths handles = at start or end of token") {
  std::filesystem::path const cache_root{ "/cache" };
  // = at start (no key)
  CHECK(envy::util_simplify_cache_paths("=/cache/foo", cache_root) == "=/cache/foo");
  // = at end (no value)
  CHECK(envy::util_simplify_cache_paths("KEY=", cache_root) == "KEY=");
  // Just =
  CHECK(envy::util_simplify_cache_paths("=", cache_root) == "=");
}

TEST_CASE("util_simplify_cache_paths real-world ninja configure example") {
  std::filesystem::path const cache_root{ "/Users/test/Library/Caches/envy" };
  envy::product_map_t const products{ { "gtest", "lib/gtest" } };
  std::string const cmd{
    "python3 configure.py --bootstrap "
    "--gtest-source-dir=/Users/test/Library/Caches/envy/assets/gtest@v1/abc123/lib/gtest"
  };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root, products) ==
        "python3 configure.py --bootstrap --gtest-source-dir=gtest");
}

TEST_CASE("util_simplify_cache_paths handles trailing slash in key=value") {
  std::filesystem::path const cache_root{ "/cache" };
  // Path with trailing slash (common from util_path_with_separator)
  std::string const cmd{ "./configure --prefix=/cache/pkg@v1/install/" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root) ==
        "./configure --prefix=install");
}

TEST_CASE("util_simplify_cache_paths handles trailing slash standalone path") {
  std::filesystem::path const cache_root{ "/cache" };
  std::string const cmd{ "/cache/pkg@v1/install/ --flag" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root) == "install --flag");
}

TEST_CASE("util_simplify_cache_paths treats semicolon as separator") {
  std::filesystem::path const cache_root{ "/cache" };
  // Semicolons separate commands after flattening; must not be included in path
  std::string const cmd{ "./configure --prefix=/cache/pkg@v1/install/; make -j" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root) ==
        "./configure --prefix=install; make -j");
}

TEST_CASE("util_simplify_cache_paths handles multiple semicolon-separated commands") {
  std::filesystem::path const cache_root{ "/cache" };
  std::string const cmd{ "/cache/bin/cmd1; /cache/bin/cmd2; /cache/bin/cmd3" };
  CHECK(envy::util_simplify_cache_paths(cmd, cache_root) == "cmd1; cmd2; cmd3");
}

// util_path_with_separator tests

// util_absolute_path tests

#ifdef _WIN32
TEST_CASE("util_absolute_path resolves relative path") {
  CHECK(envy::util_absolute_path("foo\\bar.txt", "C:\\anchor\\dir") ==
        "C:\\anchor\\dir\\foo\\bar.txt");
}

TEST_CASE("util_absolute_path normalizes parent refs") {
  CHECK(envy::util_absolute_path("..\\sibling\\file.txt", "C:\\anchor\\dir") ==
        "C:\\anchor\\sibling\\file.txt");
}

TEST_CASE("util_absolute_path normalizes dot refs") {
  CHECK(envy::util_absolute_path(".\\foo\\..\\bar.txt", "C:\\anchor\\dir") ==
        "C:\\anchor\\dir\\bar.txt");
}

TEST_CASE("util_absolute_path throws on absolute path") {
  CHECK_THROWS_WITH(envy::util_absolute_path("C:\\abs\\path", "D:\\anchor"),
                    doctest::Contains("path must be relative"));
}

TEST_CASE("util_absolute_path throws on relative anchor") {
  CHECK_THROWS_WITH(envy::util_absolute_path("foo.txt", "relative\\anchor"),
                    doctest::Contains("anchor must be absolute"));
}

TEST_CASE("util_absolute_path handles empty relative") {
  CHECK(envy::util_absolute_path("", "C:\\anchor\\dir") == "C:\\anchor\\dir\\");
}
#else
TEST_CASE("util_absolute_path resolves relative path") {
  CHECK(envy::util_absolute_path("foo/bar.txt", "/anchor/dir") ==
        "/anchor/dir/foo/bar.txt");
}

TEST_CASE("util_absolute_path normalizes parent refs") {
  CHECK(envy::util_absolute_path("../sibling/file.txt", "/anchor/dir") ==
        "/anchor/sibling/file.txt");
}

TEST_CASE("util_absolute_path normalizes dot refs") {
  CHECK(envy::util_absolute_path("./foo/../bar.txt", "/anchor/dir") ==
        "/anchor/dir/bar.txt");
}

TEST_CASE("util_absolute_path throws on absolute path") {
  CHECK_THROWS_WITH(envy::util_absolute_path("/abs/path", "/anchor"),
                    doctest::Contains("path must be relative"));
}

TEST_CASE("util_absolute_path throws on relative anchor") {
  CHECK_THROWS_WITH(envy::util_absolute_path("foo.txt", "relative/anchor"),
                    doctest::Contains("anchor must be absolute"));
}

TEST_CASE("util_absolute_path handles empty relative") {
  CHECK(envy::util_absolute_path("", "/anchor/dir") == "/anchor/dir/");
}
#endif

// util_normalized_path tests
//
// envy assembles paths from a cache root, manifest text and Lua fragments, so a join
// can mix separators — "C:/cache/pkg" / "file" is "C:/cache/pkg\\file". Anything a
// spec sees goes through util_normalized_path, so envy.package and envy.product agree
// on spelling and no path envy emits is mixed.

TEST_CASE("util_normalized_path leaves an empty path empty") {
  CHECK(envy::util_normalized_path(std::filesystem::path{}).empty());
}

TEST_CASE("util_normalized_path emits only the preferred separator") {
  char const sep{ static_cast<char>(std::filesystem::path::preferred_separator) };
  char const other{ sep == '/' ? '\\' : '/' };

  // A joined path is where mixing appears: the base keeps whatever spelling it was
  // given, and operator/ appends the preferred separator.
  std::filesystem::path base{ std::string{ "a" } + other + "b" };
  std::string const joined{ envy::util_normalized_path(base / "c") };

  // On Windows both characters are separators, so all three components are joined by
  // the preferred one. On POSIX a backslash is an ordinary filename character, so the
  // base is one component and there is nothing to convert.
#ifdef _WIN32
  CHECK(joined == "a\\b\\c");
  CHECK(joined.find('/') == std::string::npos);
#else
  CHECK(joined == std::string{ "a" } + other + "b/c");
#endif
}

TEST_CASE("util_normalized_path is idempotent") {
  std::filesystem::path const p{ std::filesystem::path{ "x" } / "y" / "z" };
  std::string const once{ envy::util_normalized_path(p) };
  CHECK(envy::util_normalized_path(std::filesystem::path{ once }) == once);
}

#ifdef _WIN32
TEST_CASE("util_normalized_path converts forward slashes on Windows") {
  CHECK(envy::util_normalized_path("C:/cache/pkg/file") == "C:\\cache\\pkg\\file");
}

TEST_CASE("util_path_with_separator normalizes before appending") {
  CHECK(envy::util_path_with_separator("C:/cache/pkg") == "C:\\cache\\pkg\\");
  // Already separator-terminated, in the other spelling: normalized, not doubled.
  CHECK(envy::util_path_with_separator("C:/cache/pkg/") == "C:\\cache\\pkg\\");
}
#endif

// util_path_with_separator tests

TEST_CASE("util_path_with_separator handles empty path") {
  CHECK(envy::util_path_with_separator(std::filesystem::path{}) == "");
}

TEST_CASE("util_path_with_separator adds separator to path without one") {
  std::filesystem::path const p{ "path/to/dir" };
  std::string const result{ envy::util_path_with_separator(p) };
  CHECK(!result.empty());
  // Result should end with preferred separator
  char const sep{ static_cast<char>(std::filesystem::path::preferred_separator) };
  CHECK(result.back() == sep);
  // Content preserved, in normalized spelling — p.string() keeps whatever separators
  // it was constructed with, which on Windows is not what a spec is handed.
  CHECK(result.substr(0, result.size() - 1) == envy::util_normalized_path(p));
}

TEST_CASE("util_path_with_separator normalizes a trailing separator without doubling") {
  // Where '/' is already the preferred separator this is a no-op; on Windows the
  // trailing '/' is a separator too, so it is converted rather than appended to.
  std::filesystem::path const p{ "path/to/dir/" };
  std::string const result{ envy::util_path_with_separator(p) };
  char const sep{ static_cast<char>(std::filesystem::path::preferred_separator) };
  CHECK(result.size() == p.string().size());
  CHECK(result.back() == sep);
  CHECK(result.find(sep == '/' ? '\\' : '/') == std::string::npos);
}

#ifdef _WIN32
TEST_CASE("util_path_with_separator preserves path already ending with backslash") {
  std::filesystem::path const p{ "C:\\path\\to\\dir\\" };
  std::string const result{ envy::util_path_with_separator(p) };
  CHECK(result.back() == '\\');
}

TEST_CASE("util_path_with_separator adds backslash on Windows") {
  std::filesystem::path const p{ "C:\\path\\to\\dir" };
  std::string const result{ envy::util_path_with_separator(p) };
  CHECK(result.back() == '\\');
  CHECK(result == "C:\\path\\to\\dir\\");
}
#else
TEST_CASE("util_path_with_separator adds forward slash on POSIX") {
  std::filesystem::path const p{ "/path/to/dir" };
  std::string const result{ envy::util_path_with_separator(p) };
  CHECK(result.back() == '/');
  CHECK(result == "/path/to/dir/");
}
#endif

TEST_CASE("util_path_with_separator enables correct Lua concatenation") {
  // This test verifies the primary use case: Lua's `dir .. "filename"` produces correct
  // paths
  std::filesystem::path const fetch_dir{ "/some/fetch/dir" };
  std::string const fetch_dir_str{ envy::util_path_with_separator(fetch_dir) };
  std::string const filename{ "test.tar.gz" };

  // Simulating Lua's .. operator
  std::string const full_path{ fetch_dir_str + filename };

  // The result should be a valid path with separator between dir and filename
  CHECK(full_path.find("dirtest") == std::string::npos);  // No missing separator
  // Should have either /test.tar.gz or \test.tar.gz (platform-dependent)
  bool const has_forward_slash{ full_path.find("/test.tar.gz") != std::string::npos };
  bool const has_backslash{ full_path.find("\\test.tar.gz") != std::string::npos };
  CHECK((has_forward_slash || has_backslash));
}

// --- util_escape_json_string tests ---

TEST_CASE("util_escape_json_string handles empty string") {
  CHECK(envy::util_escape_json_string("") == "");
}

TEST_CASE("util_escape_json_string passes through plain ASCII") {
  CHECK(envy::util_escape_json_string("hello world") == "hello world");
}

TEST_CASE("util_escape_json_string passes through digits and punctuation") {
  CHECK(envy::util_escape_json_string("abc123!@#$%^&*()") == "abc123!@#$%^&*()");
}

TEST_CASE("util_escape_json_string escapes backslash") {
  CHECK(envy::util_escape_json_string("a\\b") == "a\\\\b");
  CHECK(envy::util_escape_json_string("\\") == "\\\\");
  CHECK(envy::util_escape_json_string("\\\\") == "\\\\\\\\");
}

TEST_CASE("util_escape_json_string escapes double quote") {
  CHECK(envy::util_escape_json_string("say \"hi\"") == "say \\\"hi\\\"");
  CHECK(envy::util_escape_json_string("\"") == "\\\"");
}

TEST_CASE("util_escape_json_string escapes newline") {
  CHECK(envy::util_escape_json_string("line1\nline2") == "line1\\nline2");
  CHECK(envy::util_escape_json_string("\n") == "\\n");
}

TEST_CASE("util_escape_json_string escapes carriage return") {
  CHECK(envy::util_escape_json_string("line1\rline2") == "line1\\rline2");
  CHECK(envy::util_escape_json_string("\r\n") == "\\r\\n");
}

TEST_CASE("util_escape_json_string escapes tab") {
  CHECK(envy::util_escape_json_string("col1\tcol2") == "col1\\tcol2");
}

TEST_CASE("util_escape_json_string escapes backspace") {
  CHECK(envy::util_escape_json_string("a\bb") == "a\\bb");
}

TEST_CASE("util_escape_json_string escapes form feed") {
  CHECK(envy::util_escape_json_string("a\fb") == "a\\fb");
}

TEST_CASE("util_escape_json_string escapes null byte via unicode") {
  std::string input{ "a" };
  input += '\0';
  input += 'b';
  CHECK(envy::util_escape_json_string(input) == "a\\u0000b");
}

TEST_CASE("util_escape_json_string escapes other control chars via unicode") {
  // \x01 through \x1f (excluding named escapes) should produce \u00xx
  CHECK(envy::util_escape_json_string(std::string(1, '\x01')) == "\\u0001");
  CHECK(envy::util_escape_json_string(std::string(1, '\x02')) == "\\u0002");
  CHECK(envy::util_escape_json_string(std::string(1, '\x1f')) == "\\u001f");
  CHECK(envy::util_escape_json_string(std::string(1, '\x1e')) == "\\u001e");
  CHECK(envy::util_escape_json_string(std::string(1, '\x11')) == "\\u0011");
}

TEST_CASE("util_escape_json_string does not escape 0x20 (space)") {
  CHECK(envy::util_escape_json_string(" ") == " ");
  CHECK(envy::util_escape_json_string("a b") == "a b");
}

TEST_CASE("util_escape_json_string handles multiple escapes in sequence") {
  std::string input;
  input += '"';
  input += '\\';
  input += '\n';
  input += '\r';
  input += '\t';
  CHECK(envy::util_escape_json_string(input) == "\\\"\\\\\\n\\r\\t");
}

TEST_CASE("util_escape_json_string handles path-like strings") {
  CHECK(envy::util_escape_json_string("/usr/bin/tool") == "/usr/bin/tool");
  CHECK(envy::util_escape_json_string("C:\\Users\\foo") == "C:\\\\Users\\\\foo");
}

TEST_CASE("util_escape_json_string handles Windows backslash paths") {
  // Double-check Windows-style paths produce doubled backslashes
  std::string const input{ "D:\\a\\envy\\envy" };
  std::string const expected{ "D:\\\\a\\\\envy\\\\envy" };
  CHECK(envy::util_escape_json_string(input) == expected);
}

TEST_CASE("util_escape_json_string handles UTF-8 pass-through") {
  // UTF-8 multibyte sequences (bytes >= 0x80) should pass through unmodified
  CHECK(envy::util_escape_json_string("café") == "café");
  CHECK(envy::util_escape_json_string("日本語") == "日本語");
}

TEST_CASE("util_escape_json_string all named escapes are distinct") {
  // Verify each named escape produces its specific output
  CHECK(envy::util_escape_json_string(std::string(1, '\b')) == "\\b");
  CHECK(envy::util_escape_json_string(std::string(1, '\f')) == "\\f");
  CHECK(envy::util_escape_json_string(std::string(1, '\n')) == "\\n");
  CHECK(envy::util_escape_json_string(std::string(1, '\r')) == "\\r");
  CHECK(envy::util_escape_json_string(std::string(1, '\t')) == "\\t");
  CHECK(envy::util_escape_json_string(std::string(1, '"')) == "\\\"");
  CHECK(envy::util_escape_json_string(std::string(1, '\\')) == "\\\\");
}

TEST_CASE("util_escape_json_string all control chars below 0x20 are escaped") {
  for (int i{ 0 }; i < 0x20; ++i) {
    std::string input(1, static_cast<char>(i));
    std::string const result{ envy::util_escape_json_string(input) };
    // Every control char must produce an escape sequence (starts with backslash)
    CHECK(result.size() >= 2);
    CHECK(result[0] == '\\');
  }
}

// --- util_parse_archive_filename tests ---

TEST_CASE("util_parse_archive_filename: basic valid filenames") {
  SUBCASE("standard darwin arm64") {
    auto const r{ envy::util_parse_archive_filename(
        "local.pkg@v1-darwin-arm64-blake3-abcdef0123456789") };
    REQUIRE(r.has_value());
    CHECK(r->identity == "local.pkg@v1");
    CHECK(r->platform == "darwin");
    CHECK(r->arch == "arm64");
    CHECK(r->hash_prefix == "abcdef0123456789");
  }

  SUBCASE("linux x86_64") {
    auto const r{ envy::util_parse_archive_filename(
        "arm.gcc@r2-linux-x86_64-blake3-1234567890abcdef") };
    REQUIRE(r.has_value());
    CHECK(r->identity == "arm.gcc@r2");
    CHECK(r->platform == "linux");
    CHECK(r->arch == "x86_64");
    CHECK(r->hash_prefix == "1234567890abcdef");
  }

  SUBCASE("windows x86_64") {
    auto const r{ envy::util_parse_archive_filename(
        "local.uv@r0-windows-x86_64-blake3-deadbeef12345678") };
    REQUIRE(r.has_value());
    CHECK(r->identity == "local.uv@r0");
    CHECK(r->platform == "windows");
    CHECK(r->arch == "x86_64");
    CHECK(r->hash_prefix == "deadbeef12345678");
  }
}

TEST_CASE("util_parse_archive_filename: identity edge cases") {
  SUBCASE("multi-segment namespace") {
    auto const r{ envy::util_parse_archive_filename(
        "org.team.tool@v3-darwin-arm64-blake3-abcdef0123456789") };
    REQUIRE(r.has_value());
    CHECK(r->identity == "org.team.tool@v3");
  }

  SUBCASE("revision with digits") {
    auto const r{ envy::util_parse_archive_filename(
        "tool@r123-linux-x86_64-blake3-abcdef0123456789") };
    REQUIRE(r.has_value());
    CHECK(r->identity == "tool@r123");
  }

  SUBCASE("single-char namespace") {
    auto const r{ envy::util_parse_archive_filename(
        "a.b@r0-darwin-arm64-blake3-abcdef0123456789") };
    REQUIRE(r.has_value());
    CHECK(r->identity == "a.b@r0");
  }

  SUBCASE("hyphenated names") {
    auto const r{ envy::util_parse_archive_filename(
        "my-tool.sub-pkg@v1-darwin-arm64-blake3-abcdef0123456789") };
    REQUIRE(r.has_value());
    CHECK(r->identity == "my-tool.sub-pkg@v1");
  }
}

TEST_CASE("util_parse_archive_filename: platform/arch variants") {
  SUBCASE("unknown platform (forward compatibility)") {
    auto const r{ envy::util_parse_archive_filename(
        "tool@r0-freebsd-arm64-blake3-abcdef0123456789") };
    REQUIRE(r.has_value());
    CHECK(r->platform == "freebsd");
  }

  SUBCASE("unknown arch (forward compatibility)") {
    auto const r{ envy::util_parse_archive_filename(
        "tool@r0-linux-riscv64-blake3-abcdef0123456789") };
    REQUIRE(r.has_value());
    CHECK(r->arch == "riscv64");
  }
}

TEST_CASE("util_parse_archive_filename: invalid filenames return nullopt") {
  CHECK_FALSE(
      envy::util_parse_archive_filename("local.pkg-darwin-arm64-blake3-hash").has_value());

  CHECK_FALSE(envy::util_parse_archive_filename("local.pkg@v1").has_value());

  CHECK_FALSE(envy::util_parse_archive_filename("local.pkg@v1-darwin-arm64-sha256-hash")
                  .has_value());

  CHECK_FALSE(
      envy::util_parse_archive_filename("local.pkg@v1--arm64-blake3-hash").has_value());

  CHECK_FALSE(
      envy::util_parse_archive_filename("local.pkg@v1-darwin--blake3-hash").has_value());

  CHECK_FALSE(
      envy::util_parse_archive_filename("local.pkg@v1-darwin-arm64-blake3-").has_value());

  CHECK_FALSE(
      envy::util_parse_archive_filename("local.pkg@v1-darwin-arm64-blake3").has_value());

  CHECK_FALSE(envy::util_parse_archive_filename("").has_value());

  CHECK_FALSE(envy::util_parse_archive_filename("@").has_value());

  CHECK_FALSE(envy::util_parse_archive_filename("garbage-nonsense").has_value());
}

TEST_CASE("util_parse_archive_filename: empty revision after @") {
  // pkg@-darwin-arm64-blake3-hash: empty revision, first dash is delimiter
  auto const r{ envy::util_parse_archive_filename(
      "pkg@-darwin-arm64-blake3-abcdef0123456789") };
  // The parser treats everything up to the first '-' after '@' as revision.
  // With "pkg@-..." the revision is empty, identity = "pkg@".
  // This is a valid parse — the identity is unusual but parseable.
  REQUIRE(r.has_value());
  CHECK(r->identity == "pkg@");
  CHECK(r->platform == "darwin");
}

TEST_CASE("util_parse_archive_filename: multiple @ signs") {
  // First '@' is the identity delimiter; second '@' is part of revision chars
  auto const r{ envy::util_parse_archive_filename(
      "a@b@v1-darwin-arm64-blake3-abcdef0123456789") };
  // Parser finds first '@' at position 1, then looks for '-' after 'b@v1'.
  // 'b@v1' contains no '-', so it scans to the first '-' which separates revision from
  // platform.
  REQUIRE(r.has_value());
  CHECK(r->identity == "a@b@v1");
  CHECK(r->platform == "darwin");
  CHECK(r->arch == "arm64");
}

TEST_CASE("util_parse_archive_filename: no content before @") {
  auto const r{ envy::util_parse_archive_filename(
      "@v1-darwin-arm64-blake3-abcdef0123456789") };
  // Empty namespace before '@', but the parser doesn't reject it
  REQUIRE(r.has_value());
  CHECK(r->identity == "@v1");
  CHECK(r->platform == "darwin");
}

TEST_CASE("util_parse_archive_filename: revision with hyphen") {
  // Revision "rc-1" contains a hyphen — parser stops at first '-' after '@'
  auto const r{ envy::util_parse_archive_filename(
      "tool@rc-1-darwin-arm64-blake3-abcdef0123456789") };
  // Parser finds '@' then first '-' after it: identity = "tool@rc"
  // remaining = "1-darwin-arm64-blake3-abcdef0123456789"
  // platform = "1", arch = "darwin", tag = "arm64" → tag != "blake3" → nullopt
  // Actually: let's check what happens
  // After @: "rc-1-darwin-arm64-blake3-abcdef0123456789"
  // First dash: pos=2, so identity = "tool@rc", remaining = "1-darwin-arm64-blake3-..."
  // split_next: platform="1", arch="darwin", tag="arm64", hash="blake3-abcdef0123456789"
  // tag != "blake3" → nullopt
  CHECK_FALSE(r.has_value());
}

TEST_CASE("util_parse_platform_flag empty string returns native platform") {
  auto const result{ envy::util_parse_platform_flag("") };
  REQUIRE(result.size() == 1);
  CHECK(result[0] == envy::platform::native());
}

TEST_CASE("util_parse_platform_flag posix") {
  auto const result{ envy::util_parse_platform_flag("posix") };
  REQUIRE(result.size() == 1);
  CHECK(result[0] == envy::platform_id::POSIX);
}

TEST_CASE("util_parse_platform_flag windows") {
  auto const result{ envy::util_parse_platform_flag("windows") };
  REQUIRE(result.size() == 1);
  CHECK(result[0] == envy::platform_id::WINDOWS);
}

TEST_CASE("util_parse_platform_flag all returns both platforms") {
  auto const result{ envy::util_parse_platform_flag("all") };
  REQUIRE(result.size() == 2);
  CHECK(result[0] == envy::platform_id::POSIX);
  CHECK(result[1] == envy::platform_id::WINDOWS);
}

TEST_CASE("util_parse_platform_flag invalid value throws") {
  CHECK_THROWS_WITH(envy::util_parse_platform_flag("linux"),
                    doctest::Contains("invalid --platform value"));
  CHECK_THROWS_WITH(envy::util_parse_platform_flag("macos"),
                    doctest::Contains("invalid --platform value"));
}

TEST_CASE("util_parse_archive_filename: simple identity") {
  auto const r{ envy::util_parse_archive_filename(
      "arm.gcc@r2-darwin-arm64-blake3-abcdef0123456789") };
  REQUIRE(r.has_value());
  CHECK(r->identity == "arm.gcc@r2");
  CHECK(r->platform == "darwin");
  CHECK(r->arch == "arm64");
  CHECK(r->hash_prefix == "abcdef0123456789");
}

TEST_CASE("util_parse_archive_filename: hyphenated name") {
  auto const r{ envy::util_parse_archive_filename(
      "ns.my-tool@r10-linux-x86_64-blake3-0123456789abcdef") };
  REQUIRE(r.has_value());
  CHECK(r->identity == "ns.my-tool@r10");
  CHECK(r->platform == "linux");
  CHECK(r->arch == "x86_64");
  CHECK(r->hash_prefix == "0123456789abcdef");
}

TEST_CASE("util_parse_archive_filename: windows platform") {
  auto const r{ envy::util_parse_archive_filename(
      "core.python@r1-windows-x86_64-blake3-deadbeef") };
  REQUIRE(r.has_value());
  CHECK(r->identity == "core.python@r1");
  CHECK(r->platform == "windows");
  CHECK(r->arch == "x86_64");
  CHECK(r->hash_prefix == "deadbeef");
}

TEST_CASE("util_parse_archive_filename: missing @ returns nullopt") {
  CHECK_FALSE(envy::util_parse_archive_filename("arm.gcc-r2-darwin-arm64-blake3-abcdef")
                  .has_value());
}

TEST_CASE("util_parse_archive_filename: missing variant returns nullopt") {
  CHECK_FALSE(envy::util_parse_archive_filename("arm.gcc@r2").has_value());
}

TEST_CASE("util_parse_archive_filename: wrong hash tag returns nullopt") {
  CHECK_FALSE(envy::util_parse_archive_filename("arm.gcc@r2-darwin-arm64-sha256-abcdef")
                  .has_value());
}

TEST_CASE("util_parse_archive_filename: empty hash prefix returns nullopt") {
  CHECK_FALSE(
      envy::util_parse_archive_filename("arm.gcc@r2-darwin-arm64-blake3-").has_value());
}

TEST_CASE("util_parse_archive_filename: empty platform returns nullopt") {
  CHECK_FALSE(
      envy::util_parse_archive_filename("arm.gcc@r2--arm64-blake3-abcdef").has_value());
}

TEST_CASE("util_parse_archive_filename: empty arch returns nullopt") {
  CHECK_FALSE(
      envy::util_parse_archive_filename("arm.gcc@r2-darwin--blake3-abcdef").has_value());
}

// --- util_platform_matches ---

TEST_CASE("util_platform_matches: empty constraints match everything") {
  CHECK(envy::util_platform_matches({}, "darwin", "arm64"));
  CHECK(envy::util_platform_matches({}, "linux", "x86_64"));
  CHECK(envy::util_platform_matches({}, "windows", "x86_64"));
}

TEST_CASE("util_platform_matches: OS-only match") {
  CHECK(envy::util_platform_matches({ "linux" }, "linux", "x86_64"));
  CHECK(envy::util_platform_matches({ "linux" }, "linux", "arm64"));
  CHECK_FALSE(envy::util_platform_matches({ "linux" }, "darwin", "arm64"));
}

TEST_CASE("util_platform_matches: OS-arch match") {
  CHECK(envy::util_platform_matches({ "darwin-arm64" }, "darwin", "arm64"));
  CHECK_FALSE(envy::util_platform_matches({ "darwin-arm64" }, "darwin", "x86_64"));
  CHECK_FALSE(envy::util_platform_matches({ "darwin-arm64" }, "linux", "arm64"));
}

TEST_CASE("util_platform_matches: multiple constraints") {
  std::vector<std::string> const constraints{ "linux", "darwin-arm64" };
  CHECK(envy::util_platform_matches(constraints, "linux", "x86_64"));
  CHECK(envy::util_platform_matches(constraints, "darwin", "arm64"));
  CHECK_FALSE(envy::util_platform_matches(constraints, "darwin", "x86_64"));
  CHECK_FALSE(envy::util_platform_matches(constraints, "windows", "x86_64"));
}

// --- util_platform_intersect ---

TEST_CASE("util_platform_intersect: both empty yields empty") {
  CHECK(envy::util_platform_intersect({}, {}).empty());
}

TEST_CASE("util_platform_intersect: one empty returns the other") {
  std::vector<std::string> const v{ "linux", "darwin" };
  CHECK(envy::util_platform_intersect(v, {}) == v);
  CHECK(envy::util_platform_intersect({}, v) == v);
}

TEST_CASE("util_platform_intersect: disjoint yields empty") {
  CHECK(envy::util_platform_intersect({ "linux" }, { "darwin" }).empty());
}

TEST_CASE("util_platform_intersect: overlapping yields intersection") {
  auto const result{ envy::util_platform_intersect({ "linux", "darwin" },
                                                   { "darwin", "windows" }) };
  CHECK(result.size() == 1);
  CHECK(result[0] == "darwin");
}

// --- util_platform_matches_platform_id ---

TEST_CASE("util_platform_matches_platform_id: empty constraints match all") {
  CHECK(envy::util_platform_matches_platform_id({}, envy::platform_id::POSIX));
  CHECK(envy::util_platform_matches_platform_id({}, envy::platform_id::WINDOWS));
}

TEST_CASE("util_platform_matches_platform_id: POSIX covers darwin and linux") {
  CHECK(envy::util_platform_matches_platform_id({ "darwin" }, envy::platform_id::POSIX));
  CHECK(envy::util_platform_matches_platform_id({ "linux" }, envy::platform_id::POSIX));
  CHECK_FALSE(
      envy::util_platform_matches_platform_id({ "windows" }, envy::platform_id::POSIX));
}

TEST_CASE("util_platform_matches_platform_id: WINDOWS covers windows") {
  CHECK(
      envy::util_platform_matches_platform_id({ "windows" }, envy::platform_id::WINDOWS));
  CHECK_FALSE(
      envy::util_platform_matches_platform_id({ "darwin" }, envy::platform_id::WINDOWS));
  CHECK_FALSE(
      envy::util_platform_matches_platform_id({ "linux" }, envy::platform_id::WINDOWS));
}

TEST_CASE("util_platform_matches_platform_id: OS-arch constraint extracts OS") {
  CHECK(envy::util_platform_matches_platform_id({ "linux-x86_64" },
                                                envy::platform_id::POSIX));
  CHECK_FALSE(envy::util_platform_matches_platform_id({ "linux-x86_64" },
                                                      envy::platform_id::WINDOWS));
}

TEST_CASE("util_is_safe_path_component") {
  CHECK(envy::util_is_safe_path_component("tools.gcc@14.2"));
  CHECK(envy::util_is_safe_path_component("a-b_c.d@1"));
  CHECK(envy::util_is_safe_path_component("..."));  // odd but a plain filename
  CHECK(envy::util_is_safe_path_component("a..b"));

  CHECK_FALSE(envy::util_is_safe_path_component(""));
  CHECK_FALSE(envy::util_is_safe_path_component("."));
  CHECK_FALSE(envy::util_is_safe_path_component(".."));
  CHECK_FALSE(envy::util_is_safe_path_component("a/b"));
  CHECK_FALSE(envy::util_is_safe_path_component("a\\b"));
  CHECK_FALSE(envy::util_is_safe_path_component("../evil@1.0"));
  CHECK_FALSE(envy::util_is_safe_path_component("foo.bar/../../../evil@1.0"));
  CHECK_FALSE(envy::util_is_safe_path_component("C:evil"));
  CHECK_FALSE(envy::util_is_safe_path_component("a b"));
  CHECK_FALSE(envy::util_is_safe_path_component("a\tb"));

  // Non-ASCII bytes must be rejected regardless of locale (std::isalnum could
  // otherwise accept high-bit UTF-8 bytes under a non-"C" locale).
  CHECK_FALSE(envy::util_is_safe_path_component("caf\xc3\xa9"));   // "café" (UTF-8)
  CHECK_FALSE(envy::util_is_safe_path_component("\xe4\xbd\xa0"));  // "你" (UTF-8)
  CHECK_FALSE(envy::util_is_safe_path_component(std::string_view{ "\xff\xfe", 2 }));
}

TEST_CASE("util_ascii_is_alpha/alnum are locale-independent ASCII only") {
  CHECK(envy::util_ascii_is_alpha('a'));
  CHECK(envy::util_ascii_is_alpha('Z'));
  CHECK(envy::util_ascii_is_alnum('0'));
  CHECK_FALSE(envy::util_ascii_is_alpha('0'));
  CHECK_FALSE(envy::util_ascii_is_alpha('_'));
  CHECK_FALSE(envy::util_ascii_is_alnum(' '));
  CHECK_FALSE(envy::util_ascii_is_alpha(static_cast<char>(0xc3)));  // UTF-8 lead byte
  CHECK_FALSE(envy::util_ascii_is_alnum(static_cast<char>(0xe9)));
}

TEST_CASE("util_canonical_path makes a relative path absolute") {
  namespace fs = std::filesystem;
  auto const abs{ envy::util_canonical_path(fs::path{ "." }) };
  CHECK(abs.is_absolute());
  CHECK(abs == fs::current_path());
}

TEST_CASE("util_canonical_path normalizes dot segments away") {
  namespace fs = std::filesystem;
  // discover() walks upward with parent_path(); a surviving "." or ".." would make it
  // revisit the same directory or skip one. The '.' shape is what envy.bat's "%~dp0."
  // hands in.
  auto const cwd{ fs::current_path() };
  REQUIRE(fs::exists(cwd / "test_data"));
  CHECK(envy::util_canonical_path(cwd / ".") == cwd);
  CHECK(envy::util_canonical_path(cwd / "test_data" / "..") == cwd);
}

TEST_CASE("util_canonical_path keeps a path whose tail does not exist") {
  namespace fs = std::filesystem;
  auto const missing{ fs::current_path() / "no-such-dir-9d3f" / "deeper" };
  CHECK(envy::util_canonical_path(missing) == missing);
}

TEST_CASE("util_canonical_path is idempotent") {
  auto const once{ envy::util_canonical_path(std::filesystem::path{ "." }) };
  CHECK(envy::util_canonical_path(once) == once);
}
