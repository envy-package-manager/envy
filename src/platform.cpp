// Platform-agnostic half of platform.h: logic that drives the per-OS hooks in
// platform_posix.cpp / platform_win.cpp but has no OS-specific code itself.

#include "platform.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <utility>

namespace envy::platform {

namespace {

// A directory is the unit of work: one queue entry per directory, tagged with
// the root it belongs to. Fan-out is the queue itself — every worker both
// enumerates and accumulates, folding a directory's files into a stack-local
// dir_size and committing three fetch_adds once per directory. A dedicated
// accumulator stage would cost more in queueing than the integer adds it
// batches, and metadata reads (getdents/FindNextFile) are the only real
// latency, so every thread should be issuing one.
struct work_item {
  dir_scan_string dir;
  std::size_t root{ 0 };
};

struct root_total {
  std::atomic<std::uint64_t> bytes{ 0 };
  std::atomic<std::uint64_t> files{ 0 };
  std::atomic<std::uint64_t> dirs{ 0 };
};

constexpr std::chrono::milliseconds kProgressInterval{ 50 };

class scan_pool : unmovable {
 public:
  scan_pool(std::vector<std::filesystem::path> const &roots,
            unsigned threads,
            dir_scan_progress const &progress)
      : totals_(roots.size()), progress_{ progress } {
    queue_.reserve(roots.size());
    for (std::size_t i{ 0 }; i < roots.size(); ++i) {
      queue_.push_back({ dir_scan_root(roots[i]), i });
    }
    pending_ = queue_.size();
    if (!pending_) { return; }

    unsigned const hw{ std::thread::hardware_concurrency() };
    unsigned const n{ threads ? threads : (hw ? hw : 4u) };

    std::vector<std::thread> workers;
    workers.reserve(n);
    for (unsigned i{ 0 }; i < n; ++i) {
      workers.emplace_back([this] { run(); });
    }
    for (auto &w : workers) { w.join(); }
  }

  std::vector<dir_size> results() const {
    std::vector<dir_size> out;
    out.reserve(totals_.size());
    for (auto const &t : totals_) {
      out.push_back({ t.bytes.load(), t.files.load(), t.dirs.load() });
    }
    return out;
  }

 private:
  void run() {
    for (;;) {
      work_item item;
      {
        std::unique_lock<std::mutex> lock{ mutex_ };
        cv_.wait(lock, [this] { return !queue_.empty() || done_; });
        if (queue_.empty()) { return; }
        item = std::move(queue_.back());  // LIFO: siblings stay hot in the cache
        queue_.pop_back();
      }

      dir_size acc{};
      dir_scan_one(item.dir, acc, [this, root = item.root](dir_scan_string child) {
        std::lock_guard<std::mutex> lock{ mutex_ };
        queue_.push_back({ std::move(child), root });
        ++pending_;        // bumped before the parent's decrement, so pending_ only
        cv_.notify_one();  // reaches zero when the whole forest is drained
      });

      auto &total{ totals_[item.root] };
      total.bytes.fetch_add(acc.bytes);
      total.files.fetch_add(acc.files);
      total.dirs.fetch_add(acc.dirs);

      // Snapshot under the lock, report outside it: the callback is arbitrary work (a
      // TUI update), and every other worker needs this mutex once per directory.
      dir_size snapshot{};
      bool report{ false };
      {
        std::lock_guard<std::mutex> lock{ mutex_ };
        running_.bytes += acc.bytes;
        running_.files += acc.files;
        running_.dirs += acc.dirs;

        if (progress_) {
          if (auto const now{ std::chrono::steady_clock::now() };
              now - last_report_ >= kProgressInterval) {
            last_report_ = now;
            snapshot = running_;
            report = true;
          }
        }

        if (!--pending_) {
          done_ = true;
          cv_.notify_all();
        }
      }

      if (report) { progress_(snapshot); }
    }
  }

  std::vector<root_total> totals_;
  dir_scan_progress progress_;
  dir_size running_{};
  std::chrono::steady_clock::time_point last_report_{};
  std::vector<work_item> queue_;
  std::size_t pending_{ 0 };
  bool done_{ false };
  std::mutex mutex_;
  std::condition_variable cv_;
};

}  // namespace

std::vector<dir_size> dir_sizes(std::vector<std::filesystem::path> const &roots,
                                unsigned threads,
                                dir_scan_progress const &progress) {
  return scan_pool{ roots, threads, progress }.results();
}

}  // namespace envy::platform
