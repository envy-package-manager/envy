#include "tree_hash.h"

#include "file_read.h"
#include "glob.h"
#include "platform.h"
#include "util.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace envy {

namespace {

// Part of the stamped digest format: append, never renumber.
constexpr unsigned char kind_tag(tree_entry_kind k) {
  switch (k) {
    case tree_entry_kind::FILE: return 'f';
    case tree_entry_kind::DIRECTORY: return 'd';
    case tree_entry_kind::SYMLINK: return 'l';
  }
  return '?';
}

// Scan this directory, or hash this file. Files queue rather than being hashed by
// whoever enumerated them, or one directory of big files hashes on one thread.
struct work_item {
  tree_scan_string native;
  std::string relpath;  // '/'-joined, empty for the root
  tree_entry_kind kind{ tree_entry_kind::DIRECTORY };
  bool executable{ false };
  std::uint64_t size{ 0 };
};

struct walk_entry {
  tree_entry e;
  blake3_t digest{};  // FILE only, and only when contents were hashed
};

// Adds its lifetime to `into` and subtracts it from `outof`, which is how read time is
// reported net of the hashing inside it. Null stats means no clock reads at all.
class timer : uncopyable {
 public:
  timer(tree_hash_stats const *on, std::uint64_t *into, std::uint64_t *outof = nullptr)
      : into_{ on ? into : nullptr }, outof_{ on ? outof : nullptr } {
    if (into_) { start_ = std::chrono::steady_clock::now(); }
  }
  ~timer() {
    if (!into_) { return; }
    auto const elapsed{ static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start_)
            .count()) };
    *into_ += elapsed;
    if (outof_) { *outof_ -= elapsed; }
  }

 private:
  std::uint64_t *into_;
  std::uint64_t *outof_;
  std::chrono::steady_clock::time_point start_;
};

// Buffers one worker reuses across items, so the hot loop allocates nothing it can keep.
struct scratch {
  unsigned worker{ 0 };
  std::vector<walk_entry> out;
  std::vector<work_item> work;
  std::vector<tree_scan_entry> children;
  std::uint64_t dirs{ 0 }, files{ 0 }, bytes{ 0 };
  std::uint64_t scan_ns{ 0 }, read_ns{ 0 }, hash_ns{ 0 }, wait_ns{ 0 };
};

// A LIFO work queue drained by N workers. LIFO keeps a worker on the siblings its
// directory read just warmed.
class walker : unmovable {
 public:
  walker(tree_scan_string root,
         tree_filter const &filter,
         bool hash_contents,
         unsigned n,
         tree_hash_stats *stats)
      : filter_{ filter }, hash_contents_{ hash_contents }, stats_{ stats } {
    queue_.push_back({ .native = std::move(root) });
    pending_ = 1;

    if (stats_) {
      stats_->threads = n;
      stats_->files_per_worker.assign(n, 0);
      stats_->bytes_per_worker.assign(n, 0);
    }

    std::vector<std::thread> workers;
    workers.reserve(n);
    for (unsigned i{ 0 }; i < n; ++i) {
      workers.emplace_back([this, i] { run(i); });
    }
    for (auto &w : workers) { w.join(); }

    if (error_) { std::rethrow_exception(error_); }
    add_missing_ancestors();
    std::ranges::sort(entries_, {}, [](walk_entry const &w) -> std::string const & {
      return w.e.relpath;
    });
  }

  std::vector<walk_entry> take() { return std::move(entries_); }

 private:
  // A filter can select a file without selecting the directory holding it, but a copy of
  // that set still has to create the directory. Adding those ancestors here is what makes
  // the digest of a selection equal the digest of a copy of it -- without them a spec
  // like VENDOR = {"**/*.h"} redeploys on every run.
  void add_missing_ancestors() {
    std::unordered_set<std::string_view> present;
    present.reserve(entries_.size());
    for (auto const &w : entries_) {
      if (w.e.kind == tree_entry_kind::DIRECTORY) { present.insert(w.e.relpath); }
    }

    std::vector<std::string> missing;
    for (std::size_t i{ 0 }, n{ entries_.size() }; i < n; ++i) {
      std::string_view path{ entries_[i].e.relpath };
      for (auto slash{ path.rfind('/') }; slash != std::string_view::npos;
           slash = path.rfind('/')) {
        path = path.substr(0, slash);
        if (!present.insert(path).second) { break; }  // this ancestor and all above it
        missing.emplace_back(path);
      }
    }

    // `present` holds views into `missing`, so it dies with this scope, not before.
    for (auto &relpath : missing) {
      entries_.push_back(
          { .e = { .relpath = std::move(relpath), .kind = tree_entry_kind::DIRECTORY } });
    }
  }

 public:
 private:
  void run(unsigned worker) {
    // Results and scratch buffers are per worker; the shared lock carries only queue
    // accounting.
    scratch s;
    s.worker = worker;

    for (;;) {
      work_item item;
      {
        timer const wait{ stats_, &s.wait_ns };
        std::unique_lock lock{ mutex_ };
        cv_.wait(lock, [this] { return !queue_.empty() || done_; });
        if (queue_.empty()) { break; }
        item = std::move(queue_.back());
        queue_.pop_back();
      }

      try {
        if (item.kind == tree_entry_kind::DIRECTORY) {
          scan(item, s);
        } else {
          hash_file(item, s);
        }
      } catch (...) {
        // A partial answer is a wrong answer, so the first failure stops the whole walk.
        std::lock_guard const lock{ mutex_ };
        if (!error_) { error_ = std::current_exception(); }
        done_ = true;
        queue_.clear();
        cv_.notify_all();
        return;
      }
    }

    std::lock_guard const lock{ mutex_ };
    entries_.insert(entries_.end(),
                    std::make_move_iterator(s.out.begin()),
                    std::make_move_iterator(s.out.end()));
    if (stats_) {
      stats_->dirs += s.dirs;
      stats_->scan_ns += s.scan_ns;
      stats_->read_ns += s.read_ns;
      stats_->hash_ns += s.hash_ns;
      stats_->wait_ns += s.wait_ns;
      stats_->files_per_worker[s.worker] = s.files;
      stats_->bytes_per_worker[s.worker] = s.bytes;
    }
  }

  void scan(work_item const &item, scratch &s) {
    auto &children{ s.children };
    {
      timer const t{ stats_, &s.scan_ns };
      tree_scan_one(item.native, children);
    }
    ++s.dirs;

    auto &out{ s.out };
    auto &work{ s.work };
    work.clear();
    work.reserve(children.size());
    out.reserve(out.size() + children.size());

    for (auto const &c : children) {
      // Built with one allocation instead of the three an `a + "/" + b` chain costs.
      // Both strings are handed downstream, so neither can be a reused buffer.
      auto const name{ tree_scan_utf8(c.name) };
      std::string relpath;
      relpath.reserve(item.relpath.size() + 1 + name.size());
      relpath += item.relpath;
      if (!item.relpath.empty()) { relpath += '/'; }
      relpath += name;

      auto native{ tree_scan_join(item.native, c.name) };
      bool const selected{ filter_.selects(relpath) };

      // Descend even when the filter rejects the directory: "!build" must not hide a
      // selected "build/keep.txt".
      if (c.kind == tree_entry_kind::DIRECTORY) {
        work.push_back({ .native = std::move(native), .relpath = relpath });
        if (selected) {
          out.push_back({ .e = { .relpath = std::move(relpath),
                                 .kind = tree_entry_kind::DIRECTORY } });
        }
        continue;
      }
      if (!selected) { continue; }

      if (c.kind == tree_entry_kind::SYMLINK) {
        // One readlink, too cheap to pay a queue round trip for.
        out.push_back({ .e = { .relpath = std::move(relpath),
                               .kind = tree_entry_kind::SYMLINK,
                               .symlink_target = tree_scan_link_target(native) } });
        continue;
      }

      if (!hash_contents_) {
        out.push_back({ .e = { .relpath = std::move(relpath),
                               .kind = tree_entry_kind::FILE,
                               .executable = c.executable,
                               .size = c.size } });
        continue;
      }
      work.push_back({ .native = std::move(native),
                       .relpath = std::move(relpath),
                       .kind = tree_entry_kind::FILE,
                       .executable = c.executable,
                       .size = c.size });
    }

    publish(&work);
  }

  void hash_file(work_item const &item, scratch &s) {
    blake3_stream h;
    {
      timer const t{ stats_, &s.read_ns };  // hashing subtracts itself out below
      file_read_chunks(item.native, item.size, [&](void const *p, std::size_t n) {
        timer const inner{ stats_, &s.hash_ns, &s.read_ns };
        h.update(p, n);
      });
    }
    ++s.files;
    s.bytes += item.size;
    s.out.push_back({ .e = { .relpath = std::move(const_cast<work_item &>(item).relpath),
                             .kind = tree_entry_kind::FILE,
                             .executable = item.executable,
                             .size = item.size },
                      .digest = h.finalize() });
    publish();
  }

  // One lock per item. pending_ counts new work before this item's decrement, so it
  // only reaches zero once the forest is drained.
  void publish(std::vector<work_item> *work = nullptr) {
    std::size_t const added{ work ? work->size() : 0 };
    std::lock_guard const lock{ mutex_ };
    pending_ += added;
    if (work) {
      queue_.insert(queue_.end(),
                    std::make_move_iterator(work->begin()),
                    std::make_move_iterator(work->end()));
    }
    if (!--pending_) { done_ = true; }
    if (added || done_) { cv_.notify_all(); }
  }

  tree_filter const &filter_;
  bool hash_contents_;
  tree_hash_stats *stats_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<work_item> queue_;
  std::size_t pending_{ 0 };
  bool done_{ false };
  std::exception_ptr error_;
  std::vector<walk_entry> entries_;
};

void require_directory(std::filesystem::path const &root, char const *who) {
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec) || ec) {
    throw std::runtime_error(std::string(who) + ": not a directory: " + root.string());
  }
}

}  // namespace

tree_hash_result tree_hash(std::filesystem::path const &root,
                           tree_filter const &filter,
                           unsigned threads,
                           tree_hash_stats *stats,
                           std::vector<tree_entry> *out_entries) {
  require_directory(root, "tree_hash");
  auto const started{ std::chrono::steady_clock::now() };
  thread_budget const budget{ threads };
  auto entries{
    walker{ tree_scan_root(root), filter, true, budget.threads(), stats }.take()
  };

  timer const fold_timer{ stats, stats ? &stats->fold_ns : nullptr };
  tree_hash_result result{};
  blake3_stream fold;
  for (auto const &[e, digest] : entries) {
    // A NUL after the path, then a fixed-width payload per kind: no two trees can splice
    // their fields into the same byte stream.
    fold.update(e.relpath.data(), e.relpath.size());
    unsigned char const head[]{ '\0',
                                kind_tag(e.kind),
                                static_cast<unsigned char>(e.executable) };
    fold.update(head, sizeof(head));
    switch (e.kind) {
      case tree_entry_kind::DIRECTORY: break;
      case tree_entry_kind::SYMLINK:
        fold.update(e.symlink_target.data(), e.symlink_target.size());
        fold.update("", 1);
        break;
      case tree_entry_kind::FILE:
        fold.update(digest.data(), digest.size());
        ++result.files;
        result.bytes += e.size;
        break;
    }
  }
  result.digest = fold.finalize();
  if (out_entries) {
    out_entries->clear();
    out_entries->reserve(entries.size());
    for (auto &w : entries) { out_entries->push_back(std::move(w.e)); }
  }
  if (stats) {
    stats->wall_ns =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count());
  }
  return result;
}

bool tree_remove_listed(std::filesystem::path const &root,
                        std::vector<tree_entry> const &entries,
                        unsigned threads) {
  namespace fs = std::filesystem;

  std::vector<tree_entry const *> files;
  files.reserve(entries.size());
  for (auto const &e : entries) {
    if (e.kind != tree_entry_kind::DIRECTORY) { files.push_back(&e); }
  }

  std::atomic_bool ok{ true };
  std::atomic<std::size_t> next{ 0 };
  auto const worker{ [&] {
    constexpr std::size_t kSlice{ 64 };
    for (std::size_t first{ next.fetch_add(kSlice) }; first < files.size() && ok.load();
         first = next.fetch_add(kSlice)) {
      for (std::size_t i{ first }, last{ std::min(first + kSlice, files.size()) };
           i < last;
           ++i) {
        if (!platform::remove_file(root / fs::path{ files[i]->relpath })) {
          ok.store(false);
          return;
        }
      }
    }
  } };

  unsigned const n{ std::max(1u, std::min<unsigned>(threads, 1 + files.size() / 64)) };
  std::vector<std::thread> pool;
  pool.reserve(n - 1);
  for (unsigned i{ 1 }; i < n; ++i) { pool.emplace_back(worker); }
  worker();
  for (auto &t : pool) { t.join(); }
  if (!ok.load()) { return false; }

  // The listing is sorted, so reverse order puts every child before its parent: one
  // rmdir each, and no tree to rediscover.
  for (auto it{ entries.rbegin() }; it != entries.rend(); ++it) {
    if (it->kind != tree_entry_kind::DIRECTORY) { continue; }
    if (!platform::remove_empty_dir(root / fs::path{ it->relpath })) { return false; }
  }
  return platform::remove_empty_dir(root);
}

bool tree_remove(std::filesystem::path const &root, unsigned threads) {
  namespace fs = std::filesystem;

  std::error_code ec;
  auto const st{ fs::symlink_status(root, ec) };
  if (ec) { return false; }
  if (!fs::exists(st)) { return true; }  // nothing to do is not a failure

  // Not a directory, or a symlink to one: remove_all unlinks it without following, and
  // so does this. Following would delete a tree the caller never named.
  if (!fs::is_directory(st)) { return platform::remove_file(root); }

  try {
    thread_budget const budget{ threads };
    return tree_remove_listed(root, tree_list(root, {}, budget.threads()),
                              budget.threads());
  } catch (std::exception const &) {
    return false;  // unreadable somewhere; the caller's fallback decides how hard to try
  }
}

std::vector<tree_entry> tree_list(std::filesystem::path const &root,
                                  tree_filter const &filter,
                                  unsigned threads) {
  require_directory(root, "tree_list");
  thread_budget const budget{ threads };
  auto walked{
    walker{ tree_scan_root(root), filter, false, budget.threads(), nullptr }.take()
  };

  std::vector<tree_entry> entries;
  entries.reserve(walked.size());
  for (auto &w : walked) { entries.push_back(std::move(w.e)); }
  return entries;
}

}  // namespace envy
