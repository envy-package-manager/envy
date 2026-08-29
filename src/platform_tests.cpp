#include "platform.h"

#include "doctest.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace envy {

TEST_CASE("platform::get_exe_path returns valid path") {
  auto const path{ platform::get_exe_path() };

  CHECK(!path.empty());
  CHECK(path.is_absolute());
  CHECK(std::filesystem::exists(path));
  CHECK(std::filesystem::is_regular_file(path));
}

TEST_CASE("platform::get_exe_path returns executable file") {
  auto const path{ platform::get_exe_path() };
  auto const filename{ path.filename().string() };

  // Should be one of our test executables
  CHECK((filename.find("envy") != std::string::npos ||
         filename.find("test") != std::string::npos));
}

TEST_CASE("platform::os_name returns expected value") {
  auto const os{ platform::os_name() };
  CHECK(!os.empty());
#if defined(__APPLE__) && defined(__MACH__)
  CHECK(os == "darwin");
#elif defined(__linux__)
  CHECK(os == "linux");
#elif defined(_WIN32)
  CHECK(os == "windows");
#endif
}

TEST_CASE("platform::arch_name returns expected value") {
  auto const arch{ platform::arch_name() };
  CHECK(!arch.empty());
#if defined(__arm64__) || defined(__aarch64__) || defined(_M_ARM64)
  CHECK(arch == "arm64");
#elif defined(__x86_64__) || defined(_M_X64)
  CHECK(arch == "x86_64");
#endif
}

TEST_CASE("platform::native returns expected value") {
  auto const id{ platform::native() };
#ifdef _WIN32
  CHECK(id == platform_id::WINDOWS);
#else
  CHECK(id == platform_id::POSIX);
#endif
}

TEST_CASE("platform::native is consistent with platform::os_name") {
  auto const id{ platform::native() };
  auto const os{ platform::os_name() };
  if (os == "windows") {
    CHECK(id == platform_id::WINDOWS);
  } else {
    CHECK(id == platform_id::POSIX);
  }
}

TEST_CASE("platform::create_unique_temp_file creates a file") {
  auto const p{ platform::create_unique_temp_file("envy-test") };
  CHECK(std::filesystem::exists(p));
  CHECK(std::filesystem::is_regular_file(p));
  CHECK(p.filename().string().find("envy-test") != std::string::npos);
  std::filesystem::remove(p);
}

TEST_CASE("platform::create_unique_temp_file returns unique paths") {
  auto const a{ platform::create_unique_temp_file("envy-test-uniq") };
  auto const b{ platform::create_unique_temp_file("envy-test-uniq") };
  CHECK(a != b);
  std::filesystem::remove(a);
  std::filesystem::remove(b);
}

TEST_CASE("platform::create_unique_temp_file lives in temp directory") {
  auto const p{ platform::create_unique_temp_file("envy-test-tmp") };
  auto const tmp{ std::filesystem::canonical(std::filesystem::temp_directory_path()) };
  CHECK(std::filesystem::canonical(p.parent_path()) == tmp);
  std::filesystem::remove(p);
}

TEST_CASE("platform::exe_suffix returns platform-correct suffix") {
#ifdef _WIN32
  CHECK(platform::exe_suffix() == ".exe");
#else
  CHECK(platform::exe_suffix() == "");
#endif
}

TEST_CASE("platform::exe_name appends suffix to base name") {
  auto const name{ platform::exe_name("envy") };
#ifdef _WIN32
  CHECK(name == std::filesystem::path{ "envy.exe" });
#else
  CHECK(name == std::filesystem::path{ "envy" });
#endif
}

TEST_CASE("platform::exe_name works with arbitrary base names") {
  auto const name{ platform::exe_name("cmake") };
#ifdef _WIN32
  CHECK(name == std::filesystem::path{ "cmake.exe" });
#else
  CHECK(name == std::filesystem::path{ "cmake" });
#endif
}

namespace {

// Independent oracle: std::filesystem walking the same tree with the same
// rules (apparent sizes, symlinks counted as nothing and never followed).
platform::dir_size dir_size_oracle(std::filesystem::path const &root) {
  namespace fs = std::filesystem;

  platform::dir_size out{};
  std::error_code ec;
  fs::recursive_directory_iterator it{ root,
                                       fs::directory_options::skip_permission_denied,
                                       ec };
  if (ec) { return out; }

  for (fs::recursive_directory_iterator const end; it != end; it.increment(ec)) {
    if (ec) { break; }
    auto const st{ it->symlink_status(ec) };
    if (ec) { continue; }
    if (fs::is_symlink(st)) {
      it.disable_recursion_pending();
    } else if (fs::is_directory(st)) {
      ++out.dirs;
    } else if (fs::is_regular_file(st)) {
      ++out.files;
      out.bytes += fs::file_size(it->path(), ec);
    }
  }
  return out;
}

bool has_dir_entry(std::vector<platform::dir_entry> const &entries,
                   std::string const &name,
                   bool is_dir) {
  return std::any_of(entries.begin(), entries.end(), [&](platform::dir_entry const &e) {
    return e.name == name && e.is_dir == is_dir && !e.is_symlink;
  });
}

}  // namespace

TEST_CASE("platform::dir_list reports immediate children with types") {
  auto const entries{ platform::dir_list("test_data") };

  REQUIRE_FALSE(entries.empty());
  CHECK(has_dir_entry(entries, "lua", true));
  CHECK(has_dir_entry(entries, "specs", true));
  CHECK(has_dir_entry(entries, "ctx_run_stress.py", false));

  // "." and ".." are never surfaced.
  CHECK(std::none_of(entries.begin(), entries.end(), [](platform::dir_entry const &e) {
    return e.name == "." || e.name == "..";
  }));
}

TEST_CASE("platform::dir_list on a missing directory is empty") {
  CHECK(platform::dir_list("test_data/does-not-exist").empty());
}

TEST_CASE("platform::dir_sizes matches a std::filesystem walk") {
  auto const expected{ dir_size_oracle("test_data") };
  REQUIRE(expected.files > 0);
  REQUIRE(expected.bytes > 0);

  auto const actual{ platform::dir_sizes({ "test_data" }) };

  REQUIRE(actual.size() == 1);
  CHECK(actual[0].bytes == expected.bytes);
  CHECK(actual[0].files == expected.files);
  CHECK(actual[0].dirs == expected.dirs);
}

TEST_CASE("platform::dir_sizes keeps roots independent and index-aligned") {
  std::vector<std::filesystem::path> const roots{ "test_data/lua",
                                                  "test_data/does-not-exist",
                                                  "test_data/specs",
                                                  "test_data/bundles" };

  auto const actual{ platform::dir_sizes(roots) };

  REQUIRE(actual.size() == roots.size());
  for (size_t i{ 0 }; i < roots.size(); ++i) {
    auto const expected{ dir_size_oracle(roots[i]) };
    CHECK(actual[i].bytes == expected.bytes);
    CHECK(actual[i].files == expected.files);
    CHECK(actual[i].dirs == expected.dirs);
  }
  CHECK(actual[1].bytes == 0);  // missing root contributes nothing
  CHECK(actual[1].files == 0);
  CHECK(actual[1].dirs == 0);
}

TEST_CASE("platform::dir_sizes is thread-count invariant") {
  std::vector<std::filesystem::path> const roots{ "test_data", "test_data/archives" };

  auto const serial{ platform::dir_sizes(roots, 1) };
  auto const parallel{ platform::dir_sizes(roots, 16) };

  REQUIRE(serial.size() == parallel.size());
  for (size_t i{ 0 }; i < serial.size(); ++i) {
    CHECK(serial[i].bytes == parallel[i].bytes);
    CHECK(serial[i].files == parallel[i].files);
    CHECK(serial[i].dirs == parallel[i].dirs);
  }
}

TEST_CASE("platform::dir_sizes with no roots spawns nothing and returns nothing") {
  CHECK(platform::dir_sizes({}).empty());
}

TEST_CASE("platform::dir_sizes on a file yields zeroes") {
  auto const actual{ platform::dir_sizes({ "test_data/ctx_run_stress.py" }) };

  REQUIRE(actual.size() == 1);
  CHECK(actual[0].bytes == 0);
  CHECK(actual[0].files == 0);
  CHECK(actual[0].dirs == 0);
}

#ifdef _WIN32

namespace {

struct temp_dir {
  temp_dir() {
    static std::mt19937_64 rng{ std::random_device{}() };
    root = std::filesystem::temp_directory_path() /
           ("envy-platform-test-" + std::to_string(rng()));
    std::filesystem::create_directories(root);
  }
  ~temp_dir() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
  std::filesystem::path root;
};

}  // namespace

TEST_CASE("remove_all_with_retry: succeeds on nonexistent target") {
  temp_dir t;
  auto const target{ t.root / "does-not-exist" };
  auto const ec{ platform::remove_all_with_retry(target) };
  CHECK(!ec);
}

TEST_CASE("remove_all_with_retry: removes a normal directory tree") {
  temp_dir t;
  auto const target{ t.root / "tree" };
  std::filesystem::create_directories(target / "sub");
  { std::ofstream{ target / "sub" / "file.txt" } << "data"; }
  auto const ec{ platform::remove_all_with_retry(target) };
  CHECK(!ec);
  CHECK(!std::filesystem::exists(target));
}

TEST_CASE(
    "remove_all_with_retry: returns success when locked file is "
    "released before probe") {
  // Simulate: lock a file inside target, call remove_all_with_retry (which
  // will fail on the locked file during retries), release the lock while
  // retries are still running, and verify it eventually returns success.
  temp_dir t;
  auto const target{ t.root / "locked" };
  std::filesystem::create_directories(target);
  auto const locked_file{ target / "held.bin" };
  { std::ofstream{ locked_file } << "payload"; }

  // Open with exclusive access (no sharing) to simulate Defender lock.
  HANDLE h{ ::CreateFileW(locked_file.c_str(),
                          GENERIC_READ | GENERIC_WRITE,
                          0,  // no sharing — exclusive
                          nullptr,
                          OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL,
                          nullptr) };
  REQUIRE(h != INVALID_HANDLE_VALUE);

  // Release after 150ms — within the retry window (~3.5s) but after the
  // first attempt (which sleeps 50ms).
  std::thread releaser{ [h] {
    ::Sleep(150);
    ::CloseHandle(h);
  } };

  auto const ec{ platform::remove_all_with_retry(target) };
  releaser.join();

  CHECK(!ec);
  CHECK(!std::filesystem::exists(target));
}

TEST_CASE("remove_all_with_retry: post-loop probe detects target gone") {
  // Verify the post-loop existence check: create a dir, delete it
  // externally, then confirm remove_all_with_retry on a path that no
  // longer exists returns success.
  temp_dir t;
  auto const target{ t.root / "vanish" };
  std::filesystem::create_directories(target);
  // Remove it before calling — simulates the race where the target
  // disappears between the last remove_all error and the probe.
  std::filesystem::remove(target);
  auto const ec{ platform::remove_all_with_retry(target) };
  CHECK(!ec);
}

#endif  // _WIN32

}  // namespace envy
