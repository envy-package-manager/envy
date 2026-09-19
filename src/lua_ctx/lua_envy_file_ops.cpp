#include "lua_envy_file_ops.h"

#include "lua_phase_context.h"
#include "tree_hash.h"
#include "pkg.h"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace envy {
namespace {

// Resolve relative path to phase working directory (from registry, or stage_dir fallback)
std::filesystem::path resolve_relative(std::filesystem::path const &path,
                                       sol::this_state L) {
  if (path.is_absolute()) { return path; }
  phase_context const *ctx{ lua_phase_context_get(L) };
  if (ctx && ctx->run_dir) { return *ctx->run_dir / path; }
  if (ctx && ctx->p && ctx->p->lock) { return ctx->p->lock->stage_dir() / path; }
  return std::filesystem::current_path() / path;
}

}  // namespace

void lua_envy_file_ops_install(sol::table &envy_table) {
  // envy.copy(src, dst) - Copy file or directory (relative paths anchored to stage_dir)
  envy_table["copy"] =
      [](std::string const &src_str, std::string const &dst_str, sol::this_state L) {
        std::filesystem::path const src{ resolve_relative(src_str, L) };
        std::filesystem::path dst{ resolve_relative(dst_str, L) };

        if (!std::filesystem::exists(src)) {
          throw std::runtime_error("envy.copy: source not found: " + src.string());
        }

        // If copying file to directory, append filename
        if (std::filesystem::is_regular_file(src) && std::filesystem::is_directory(dst)) {
          dst = dst / src.filename();
        }

        // Both branches: std::filesystem::copy creates only dst itself, so a directory
        // destination needs its ancestors too.
        if (dst.has_parent_path()) {
          std::filesystem::create_directories(dst.parent_path());
        }

        if (std::filesystem::is_directory(src)) {
          std::filesystem::copy(src,
                                dst,
                                std::filesystem::copy_options::recursive |
                                    std::filesystem::copy_options::overwrite_existing);
        } else {
          std::filesystem::copy_file(src,
                                     dst,
                                     std::filesystem::copy_options::overwrite_existing);
        }
      };

  // envy.move(src, dst) - Move/rename file or directory
  envy_table["move"] =
      [](std::string const &src_str, std::string const &dst_str, sol::this_state L) {
        std::filesystem::path const src{ resolve_relative(src_str, L) };
        std::filesystem::path dst{ resolve_relative(dst_str, L) };

        if (!std::filesystem::exists(src)) {
          throw std::runtime_error("envy.move: source not found: " + src.string());
        }

        // If moving file to directory, append filename
        if (std::filesystem::is_regular_file(src) && std::filesystem::is_directory(dst)) {
          dst = dst / src.filename();
        }

        if (dst.has_parent_path()) {
          std::filesystem::create_directories(dst.parent_path());
        }

        std::filesystem::rename(src, dst);
      };

  // envy.remove(path) - Delete file or directory recursively
  envy_table["remove"] = [](std::string const &path_str, sol::this_state L) {
    std::filesystem::path const path{ resolve_relative(path_str, L) };
    // The throwing remove_all is the fallback, so a spec still learns when a tree it
    // asked to delete is still there.
    if (!tree_remove(path)) { std::filesystem::remove_all(path); }
  };

  // envy.exists(path) - Check if path exists
  envy_table["exists"] = [](std::string const &path_str, sol::this_state L) -> bool {
    return std::filesystem::exists(resolve_relative(path_str, L));
  };

  // envy.is_file(path) - Check if path is regular file
  envy_table["is_file"] = [](std::string const &path_str, sol::this_state L) -> bool {
    return std::filesystem::is_regular_file(resolve_relative(path_str, L));
  };

  // envy.is_dir(path) - Check if path is directory
  envy_table["is_dir"] = [](std::string const &path_str, sol::this_state L) -> bool {
    return std::filesystem::is_directory(resolve_relative(path_str, L));
  };
}

}  // namespace envy
