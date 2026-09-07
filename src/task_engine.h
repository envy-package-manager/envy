#pragma once

#include "util.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace envy {

// Generic threaded task executor. A task is an interned, keyed, linear sequence
// of steps run on a dedicated worker thread. Tasks advance toward a ratcheting
// target watermark; per-step edges block a step until another task reaches a
// watermark. Tasks may be created at any time, including from other tasks'
// step callbacks — the graph grows while it runs.
//
// Watermark semantics: watermark N is reached once the task has completed its
// first N steps. "Done" is watermark step_count. A step callback may finish the
// task early, jumping straight to done. A watermark once satisfied stays
// satisfied: a dependency that fails later never unsatisfies an earlier wait.
//
// Domain-agnostic: keys are opaque strings, steps/edges are callbacks. Blocking
// inside step callbacks is legal — workers are plain threads, not pooled.
//
// Lock ordering: engine mutex_ may be held while acquiring a task mutex, never
// the reverse. Callbacks are always invoked with no engine locks held.
//
// Deadlock watchdog: waits re-check the engine once an interval and fail the
// graph with a report naming every blocked wait if, for a whole interval, no
// worker ran, nothing advanced, and no blocked wait's predicate holds. A worker
// blocked on external I/O inside a step counts as running; one blocked in a
// task_engine wait inside a step does not (it is waiting, not working); one
// parked at its target counts as neither — a wait on it ratchets it awake.
class task_engine : unmovable {
 public:
  struct edge {
    std::string key;     // task to wait for
    int watermark{ 0 };  // completed-step count that satisfies the wait
    // Ratchet the target at least this far (max'd with watermark). Lets a
    // dependent demand extra downstream work without waiting for it — e.g. a
    // dependency runs its export step concurrently with the dependent.
    int extend_to{ 0 };
  };

  struct task_config {
    std::string key;
    int step_count{ 0 };

    // Run step `i` (0-based). Return true to finish the task early (remaining
    // steps skipped, watermark jumps to step_count). Throw to fail the task.
    std::function<bool(int step)> step;

    // Edges that must be satisfied before step `i` runs. Queried on the worker
    // thread immediately before each step, so results may grow with the graph.
    // Every returned target must already be interned (waiting on an unknown key
    // fails the task). Re-waiting a satisfied edge is expected and cheap.
    std::function<std::vector<edge>(int step)> edges;

    // Runs once on the worker thread before the step loop (e.g. to wire edges
    // that step 0 must observe). Throw to fail the task. Null = none.
    std::function<void()> on_start;

    // Invoked on the worker thread after the task fails for any reason
    // (on_start, an edge wait, a step threw, or fail_all cancelled it), before
    // waiters wake. Must not throw. Null = none.
    std::function<void()> on_failed;
  };

  // Scheduling-event callbacks (tracing). Fire on worker/caller threads with no
  // engine locks held; any null member is skipped. blocked/unblocked fire only
  // for edges that actually block.
  struct observer {
    std::function<
        void(std::string const &key, int step, std::string const &dep, int watermark)>
        blocked;
    std::function<void(std::string const &key, int step, std::string const &dep)>
        unblocked;
    std::function<void(std::string const &key, int old_target, int new_target)>
        target_extended;
  };

  explicit task_engine(observer obs = {});
  ~task_engine();  // fail_all + join_all

  // Intern a task. Returns true if created; false if the key already exists
  // (cfg is discarded — collisions are the caller's problem to detect). A task
  // interned after fail_all is born failed and never runs a callback but its
  // on_failed hook.
  bool ensure_task(task_config cfg);
  bool contains(std::string const &key) const;

  // Start the task's worker (idempotent) and ratchet its target to at least
  // `target` (to done if extend_all_to_done already ran). `before_spawn` runs
  // exactly once, only on the call that creates the thread, before the thread
  // exists (for state the worker must observe). If before_spawn or the thread
  // creation throws, the task is failed (waiters see the error; the worker-side
  // on_failed hook does NOT fire) and the exception rethrows.
  // Returns true if this call created the thread.
  bool start_task(std::string const &key,
                  int target,
                  std::function<void()> const &before_spawn = {});

  // Ratchet a task's target watermark upward (no-op if already higher).
  // Targets clamp to step_count — beyond-done is unsatisfiable.
  void extend_target(std::string const &key, int target);
  void extend_to_done(std::string const &key);
  // Also latches until join_all returns: a task started during that teardown
  // window runs to done too, so one interned there cannot park forever.
  void extend_all_to_done();

  // Block until `key` has completed at least `watermark` steps (clamped to
  // step_count, so an oversized watermark waits for done rather than hanging).
  // Throws std::runtime_error with the task's stored message if it failed short
  // of the watermark, or with the watchdog's report if the graph deadlocked.
  void wait_at(std::string const &key, int watermark);

  int completed(std::string const &key) const;
  int target(std::string const &key) const;
  int step_count(std::string const &key) const;
  bool failed(std::string const &key) const;

  // Cancel: mark every task failed and wake all waiters. Workers exit through
  // their failure path (on_failed fires), and tasks interned afterwards are
  // born failed.
  void fail_all();

  // Join all workers; tolerates tasks created while joining (rescans until a
  // pass finds nothing new — terminates because only live workers create tasks).
  void join_all();

  // (key, message) for every failed task; message may be empty. Call after
  // join_all for a stable view.
  std::vector<std::pair<std::string, std::string>> collect_failures() const;

  // Shared global condition for domain-level rendezvous (e.g. counters
  // decremented from step callbacks). Every step completion, task failure, and
  // fail_all also broadcasts it. `pred` is evaluated under the engine mutex and
  // must not block or call back into this engine; the watchdog may evaluate it
  // on another waiting thread. Throws like wait_at if the watchdog fires.
  void notify_global();
  void wait_global(std::function<bool()> const &pred);

#ifdef ENVY_UNIT_TEST
  // Test hooks: `spawn_failure_hook` throws in place of a failing std::thread
  // construction; the watchdog interval shrinks so deadlock tests stay fast.
  std::function<void()> spawn_failure_hook;
  void set_watchdog_interval(std::chrono::milliseconds interval);
#endif

 private:
  struct task {
    task_config cfg;
    std::thread worker;          // guarded by mutex: assigned by start_task, moved out
                                 // by join_all — they race when workers spawn tasks
    std::mutex mutex;            // guards worker, spawn_settled, error; pairs with cv
    std::condition_variable cv;  // target extension / failure wakeups
    std::atomic<int> completed{ 0 };
    std::atomic<int> target{ 0 };
    std::atomic_bool failed{ false };
    std::atomic_bool started{ false };
    bool spawn_settled{ false };  // start_task finished: worker assigned or
                                  // spawn aborted (guarded by mutex)
    std::string error;            // valid when failed (guarded by mutex)
  };

  // One blocked thread, so the watchdog can name who is stuck on what and tell
  // a wedged wait from one that is merely late to wake.
  struct wait_record {
    task *waiter{ nullptr };            // null: a caller thread, not a worker
    int step{ -1 };
    std::string const *dep{ nullptr };  // null: wait_global's condition
    int watermark{ 0 };
    std::function<bool()> const *pred{ nullptr };  // valid while in waits_
  };

  // What this thread is running inside this engine: set around on_start/step so
  // a wait nested there is attributed to the task and pauses its running tally.
  struct thread_ctx {
    task_engine *engine{ nullptr };
    task *t{ nullptr };
    int step{ -1 };
  };
  static thread_local thread_ctx tls_ctx_;

  task *find(std::string const &key) const;  // throws on unknown key
  void ratchet_target(task &t, int target, bool notify_observer = true);
  static std::string current_exception_message();  // call from a catch block
  void fail_task(task *t, std::string error_msg);
  void fail_all_locked(std::string const &blocked_error);
  void run_worker(task *t);
  void wait_on(task *t, int watermark, std::string const &key, task *waiter, int step);
  void wait_locked(std::unique_lock<std::mutex> &lock,
                   std::function<bool()> const &pred,
                   wait_record &rec);
  bool any_wait_ready_locked() const;
  std::string deadlock_report_locked() const;

  observer observer_;
  std::unordered_map<std::string, std::unique_ptr<task>> tasks_;
  std::vector<wait_record const *> waits_;  // guarded by mutex_; size = waiter count
  std::atomic<int> running_{ 0 };           // workers inside on_start/step
  std::atomic<std::uint64_t> progress_{ 0 };  // bumped by anything that can unwedge
  std::atomic_bool all_to_done_{ false };
  std::atomic_bool failed_all_{ false };
  std::atomic<int> watchdog_ms_{ 1000 };
  mutable std::mutex mutex_;
  std::condition_variable cv_;
};

}  // namespace envy
