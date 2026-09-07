#include "task_engine.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace envy {

thread_local task_engine::thread_ctx task_engine::tls_ctx_{};

task_engine::task_engine(observer obs) : observer_(std::move(obs)) {}

task_engine::~task_engine() {
  fail_all();
  join_all();
}

bool task_engine::ensure_task(task_config cfg) {
  std::lock_guard const lock(mutex_);
  auto const [it, inserted]{ tasks_.try_emplace(cfg.key, nullptr) };
  if (!inserted) { return false; }
  it->second = std::make_unique<task>();
  it->second->cfg = std::move(cfg);
  it->second->failed = failed_all_.load();  // interned mid-teardown: born failed
  return true;
}

bool task_engine::contains(std::string const &key) const {
  std::lock_guard const lock(mutex_);
  return tasks_.contains(key);
}

task_engine::task *task_engine::find(std::string const &key) const {
  std::lock_guard const lock(mutex_);
  auto const it{ tasks_.find(key) };
  if (it == tasks_.end()) { throw std::runtime_error("Unknown task: " + key); }
  return it->second.get();
}

void task_engine::ratchet_target(task &t, int target, bool notify_observer) {
  target = std::min(target, t.cfg.step_count);  // beyond-done is unsatisfiable
  int current{ t.target.load() };
  while (current < target) {
    if (t.target.compare_exchange_weak(current, target)) {
      {
        std::lock_guard const lock(t.mutex);
        t.cv.notify_one();
      }
      ++progress_;  // an extension can unpark a worker
      // Outside the task mutex: observers may reenter the engine, and
      // fail_all takes engine mutex -> task mutex (lock-order inversion).
      if (notify_observer && observer_.target_extended) {
        observer_.target_extended(t.cfg.key, current, target);
      }
      return;
    }
  }
}

bool task_engine::start_task(std::string const &key,
                             int target,
                             std::function<void()> const &before_spawn) {
  task *t{ find(key) };

  bool expected{ false };
  if (!t->started.compare_exchange_strong(expected, true)) {
    ratchet_target(*t, target);  // already running: extend
    return false;
  }

  try {
    if (before_spawn) { before_spawn(); }
    // Initial ratchet isn't an extension: the caller chose this target. Past
    // extend_all_to_done there is no later extension, so run straight to done.
    ratchet_target(*t, all_to_done_ ? t->cfg.step_count : target, false);
    // Under the task mutex: join_all may be reaping concurrently (workers
    // spawn tasks), and the handle hand-off must not tear.
    std::lock_guard const task_lock(t->mutex);
#ifdef ENVY_UNIT_TEST
    if (spawn_failure_hook) { spawn_failure_hook(); }
#endif
    t->worker = std::thread([this, t] { run_worker(t); });
    t->spawn_settled = true;
  } catch (...) {
    // `started` is latched and no worker will ever exist: settle the spawn so
    // join_all stops rescanning for it, fail the task so waiters see an error
    // instead of hanging, then surface to the caller. on_failed is NOT invoked
    // — it is a worker-side hook.
    {
      std::lock_guard const task_lock(t->mutex);
      t->spawn_settled = true;
    }
    fail_task(t, current_exception_message());
    throw;
  }

  ++progress_;  // a fresh worker is forward motion
  return true;
}

void task_engine::extend_target(std::string const &key, int target) {
  ratchet_target(*find(key), target);
}

void task_engine::extend_to_done(std::string const &key) {
  task *t{ find(key) };
  ratchet_target(*t, t->cfg.step_count);
}

void task_engine::extend_all_to_done() {
  all_to_done_ = true;  // latch before the snapshot: later starts self-extend
  std::vector<task *> snapshot;
  {
    std::lock_guard const lock(mutex_);
    snapshot.reserve(tasks_.size());
    for (auto const &[_, t] : tasks_) { snapshot.push_back(t.get()); }
  }
  for (auto *t : snapshot) { ratchet_target(*t, t->cfg.step_count); }
}

void task_engine::wait_at(std::string const &key, int watermark) {
  task *t{ find(key) };
  ratchet_target(*t, watermark);
  wait_on(t, std::min(watermark, t->cfg.step_count), key, nullptr, -1);
}

void task_engine::wait_on(task *t,
                          int watermark,
                          std::string const &key,
                          task *waiter,
                          int step) {
  std::unique_lock lock(mutex_);
  wait_record rec{ waiter, step, &key, watermark, nullptr };
  wait_locked(
      lock, [t, watermark] { return t->completed >= watermark || t->failed; }, rec);

  // A satisfied watermark outlives a later failure: a dependent that only
  // needed setup must not fail because its dependency died in export.
  if (t->completed >= watermark) { return; }
  lock.unlock();
  std::lock_guard const task_lock(t->mutex);
  throw std::runtime_error(t->error.empty() ? "Task failed: " + key : t->error);
}

void task_engine::wait_locked(std::unique_lock<std::mutex> &lock,
                              std::function<bool()> const &pred,
                              wait_record &rec) {
  if (pred()) { return; }  // satisfied: no wait, and nothing for the watchdog to see

  rec.pred = &pred;
  if (!rec.waiter && tls_ctx_.engine == this) {  // a wait nested inside a step
    rec.waiter = tls_ctx_.t;
    rec.step = tls_ctx_.step;
  }
  waits_.push_back(&rec);

  // Both guards run under the caller's still-held lock on mutex_, on every exit
  // path — normal return and the deadlock throw's unwinding alike.
  struct pop {
    task_engine &te;
    wait_record const &rec;
    ~pop() { std::erase(te.waits_, &rec); }
  } const popper{ *this, rec };
  struct unrun {  // blocked inside a step is waiting, not running
    task_engine *te;
    explicit unrun(task_engine &e) : te{ tls_ctx_.engine == &e ? &e : nullptr } {
      if (te) { --te->running_; }
    }
    ~unrun() {
      if (te) {
        ++te->progress_;  // resuming domain code is motion
        ++te->running_;
      }
    }
  } const paused{ *this };

  auto const interval{ std::chrono::milliseconds{ watchdog_ms_.load() } };
  auto deadline{ std::chrono::steady_clock::now() + interval };
  std::uint64_t mark{ progress_ };

  while (!pred()) {
    if (cv_.wait_until(lock, deadline) != std::cv_status::timeout) { continue; }
    if (pred()) { return; }  // a notification can land alongside the timeout
    // Watchdog: waits_ is non-empty (we are in it), for a whole interval no
    // worker ran and nothing advanced, and no blocked wait can proceed.
    if (running_ == 0 && progress_ == mark && !any_wait_ready_locked()) {
      std::string const msg{ deadlock_report_locked() };
      fail_all_locked(msg);
      throw std::runtime_error(msg);
    }
    mark = progress_;
    deadline = std::chrono::steady_clock::now() + interval;
  }
}

bool task_engine::any_wait_ready_locked() const {
  // A waiter whose predicate already holds is about to leave: not a deadlock.
  return std::ranges::any_of(waits_, [](wait_record const *w) { return (*w->pred)(); });
}

std::string task_engine::deadlock_report_locked() const {
  std::vector<std::string> lines;
  lines.reserve(waits_.size());
  for (auto const *w : waits_) {
    std::string line{ "\n  " };
    line += w->waiter ? w->waiter->cfg.key : "<caller>";
    if (w->step >= 0) { line += " step " + std::to_string(w->step); }
    line += w->dep ? " waits for " + *w->dep + "@" + std::to_string(w->watermark)
                   : " waits for a global condition";
    lines.push_back(std::move(line));
  }
  std::ranges::sort(lines);  // wake order must not change the message

  std::string msg{ "Deadlock: no task is running while " + std::to_string(lines.size()) +
                   " wait(s) are blocked:" };
  for (auto const &line : lines) { msg += line; }
  return msg;
}

int task_engine::completed(std::string const &key) const { return find(key)->completed; }

int task_engine::target(std::string const &key) const { return find(key)->target; }

int task_engine::step_count(std::string const &key) const {
  return find(key)->cfg.step_count;
}

bool task_engine::failed(std::string const &key) const { return find(key)->failed; }

void task_engine::fail_all() {
  std::lock_guard const lock(mutex_);
  fail_all_locked({});
}

void task_engine::fail_all_locked(std::string const &blocked_error) {
  failed_all_ = true;  // tasks interned from here on are born failed

  std::unordered_set<task *> blocked;  // the diagnosis lands on the stuck tasks
  if (!blocked_error.empty()) {
    for (auto const *w : waits_) {
      if (w->waiter) { blocked.insert(w->waiter); }
    }
  }

  for (auto const &[_, t] : tasks_) {
    // Hold the task mutex across the store so a worker can't evaluate its wait
    // predicate false and then sleep through the notify (lost wakeup).
    {
      std::lock_guard const task_lock(t->mutex);
      if (t->error.empty() && blocked.contains(t.get())) { t->error = blocked_error; }
      t->failed = true;
    }
    t->cv.notify_all();
  }
  ++progress_;
  cv_.notify_all();
}

void task_engine::join_all() {
  // tasks_ can grow while joining: live workers may create and start tasks.
  // Snapshot unjoined started tasks under mutex_ (unique_ptr ownership keeps
  // pointers stable across rehash), reap unlocked, rescan until a pass finds
  // nothing left. Unstarted tasks are excluded: once every started worker is
  // joined, nothing remains that could start one. A started task whose spawn
  // hasn't settled (start_task is between the CAS and the handle assignment)
  // stays in the batch for the next rescan.
  std::unordered_set<task *> joined;
  for (;;) {
    std::vector<task *> batch;
    {
      std::lock_guard const lock(mutex_);
      for (auto const &[_, t] : tasks_) {
        if (t->started && !joined.contains(t.get())) { batch.push_back(t.get()); }
      }
    }
    if (batch.empty()) {
      all_to_done_ = false;  // teardown over: new work targets what its caller asks
      return;
    }
    for (auto *t : batch) {
      std::thread w;
      bool settled{ false };
      {
        std::lock_guard const task_lock(t->mutex);
        w = std::move(t->worker);
        settled = t->spawn_settled;
      }
      if (w.joinable()) {
        w.join();  // outside the task mutex: the worker takes it while running
        joined.insert(t);
      } else if (settled) {
        joined.insert(t);  // spawn aborted (or already reaped): no thread exists
      }
    }
  }
}

std::vector<std::pair<std::string, std::string>> task_engine::collect_failures() const {
  std::vector<std::pair<std::string, std::string>> failures;
  std::lock_guard const lock(mutex_);
  for (auto const &[key, t] : tasks_) {
    if (t->failed) {
      std::lock_guard const task_lock(t->mutex);
      failures.emplace_back(key, t->error);
    }
  }
  return failures;
}

void task_engine::notify_global() {
  std::lock_guard const lock(mutex_);
  ++progress_;
  cv_.notify_all();
}

#ifdef ENVY_UNIT_TEST
void task_engine::set_watchdog_interval(std::chrono::milliseconds interval) {
  watchdog_ms_ = static_cast<int>(interval.count());
}
#endif

void task_engine::wait_global(std::function<bool()> const &pred) {
  std::unique_lock lock(mutex_);
  wait_record rec{};
  wait_locked(lock, pred, rec);
}

void task_engine::run_worker(task *t) {
  std::string const &key{ t->cfg.key };
  bool failed{ t->failed };  // interned during teardown: born failed, runs nothing

  // Domain code: counts as running for the watchdog and owns any wait nested
  // inside it. Both edges bump progress, so "nothing ran" and "ran and left"
  // are distinguishable; the tally drops after the store, so a reader that
  // sees zero has seen the store.
  struct domain_scope : unmovable {
    task_engine &te;
    thread_ctx const prev;

    domain_scope(task_engine &e, task *t, int step) : te{ e }, prev{ tls_ctx_ } {
      tls_ctx_ = { &e, t, step };
      ++te.progress_;
      ++te.running_;
    }
    ~domain_scope() {
      ++te.progress_;
      --te.running_;
      tls_ctx_ = prev;
    }
  };

  try {
    if (!failed && t->cfg.on_start) {
      domain_scope const scope{ *this, t, -1 };
      t->cfg.on_start();
    }

    while (!failed && t->completed < t->cfg.step_count) {
      if (t->failed) { failed = true; break; }
      int const done{ t->completed };

      if (done >= t->target) {  // reached target: wait for extension
        {
          std::unique_lock lock(t->mutex);
          t->cv.wait(lock, [t, done] { return t->target > done || t->failed; });
        }
        if (t->failed) { failed = true; break; }
        // target_extended fires from ratchet_target, where the extension is
        // deterministic; a worker may never park here if the extension lands
        // before its target check.
      }

      int const step{ t->completed };

      if (t->cfg.edges) {
        for (auto const &e : t->cfg.edges(step)) {
          task *dep{ find(e.key) };
          ratchet_target(*dep, std::max(e.watermark, e.extend_to));
          int const watermark{ std::min(e.watermark, dep->cfg.step_count) };
          // Observers hear only about real blocking: an edge already at its
          // watermark (or already failed) is not a wait.
          bool const blocks{ dep->completed < watermark && !dep->failed };
          if (blocks && observer_.blocked) {
            observer_.blocked(key, step, e.key, e.watermark);
          }
          wait_on(dep, watermark, e.key, t, step);
          if (blocks && observer_.unblocked) { observer_.unblocked(key, step, e.key); }
        }
      }

      bool finished_early{ false };
      if (t->cfg.step) {
        domain_scope const scope{ *this, t, step };
        finished_early = t->cfg.step(step);
      }
      t->completed = finished_early ? t->cfg.step_count : step + 1;
      notify_global();  // wake cross-task watermark waiters
    }
  } catch (...) {
    fail_task(t, current_exception_message());
    failed = true;
  }

  // fail_all cancels rather than merely flags: a worker that observes the
  // failure still owes on_failed, or domain-level state never settles.
  if (failed) {
    if (t->cfg.on_failed) { t->cfg.on_failed(); }
    notify_global();  // wake waiters again after the hook ran
  }
}

std::string task_engine::current_exception_message() {
  try {
    throw;  // rethrow the in-flight exception to inspect it
  } catch (std::exception const &e) { return e.what(); } catch (...) {
    return "unknown exception";
  }
}

void task_engine::fail_task(task *t, std::string error_msg) {
  {
    std::lock_guard const task_lock(t->mutex);
    if (t->error.empty()) { t->error = std::move(error_msg); }  // first cause wins
    t->failed = true;
  }
  t->cv.notify_all();
  notify_global();
}

}  // namespace envy
