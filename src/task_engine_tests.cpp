#include "task_engine.h"

#include "doctest.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace envy {

namespace {

// Thread-safe step recorder shared by tasks in a test.
struct step_log {
  std::mutex mutex;
  std::vector<std::string> entries;

  void record(std::string entry) {
    std::lock_guard const lock(mutex);
    entries.push_back(std::move(entry));
  }

  std::vector<std::string> snapshot() {
    std::lock_guard const lock(mutex);
    return entries;
  }

  size_t index_of(std::string const &entry) {
    std::lock_guard const lock(mutex);
    for (size_t i{ 0 }; i < entries.size(); ++i) {
      if (entries[i] == entry) { return i; }
    }
    FAIL("entry not found: ", entry);
    return 0;
  }
};

task_engine::task_config simple_task(std::string key, int steps, step_log &log) {
  task_engine::task_config cfg;
  cfg.key = key;
  cfg.step_count = steps;
  cfg.step = [key, &log](int step) {
    log.record(key + ":" + std::to_string(step));
    return false;
  };
  return cfg;
}

// Bounded wait: a hang-shaped bug must fail an assertion, never wedge the run.
bool spin_until(std::function<bool()> const &pred,
                std::chrono::milliseconds timeout = std::chrono::milliseconds{ 2000 }) {
  auto const deadline{ std::chrono::steady_clock::now() + timeout };
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) { return true; }
    std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
  }
  return pred();
}

}  // namespace

TEST_CASE("task_engine: single task runs all steps in order") {
  step_log log;
  task_engine te;
  REQUIRE(te.ensure_task(simple_task("a", 3, log)));
  te.start_task("a", 3);
  te.wait_at("a", 3);
  CHECK(log.snapshot() == std::vector<std::string>{ "a:0", "a:1", "a:2" });
  CHECK(te.completed("a") == 3);
  CHECK_FALSE(te.failed("a"));
}

TEST_CASE("task_engine: ensure_task is idempotent") {
  step_log log;
  task_engine te;
  CHECK(te.ensure_task(simple_task("a", 1, log)));
  CHECK_FALSE(te.ensure_task(simple_task("a", 5, log)));
  CHECK(te.step_count("a") == 1);
  CHECK(te.contains("a"));
  CHECK_FALSE(te.contains("b"));
}

TEST_CASE("task_engine: unknown key throws") {
  task_engine te;
  CHECK_THROWS_WITH(te.wait_at("ghost", 1), "Unknown task: ghost");
  CHECK_THROWS_AS(te.start_task("ghost", 1), std::runtime_error);
  CHECK_THROWS_AS(te.completed("ghost"), std::runtime_error);
}

TEST_CASE("task_engine: target ratchets and pauses execution") {
  step_log log;
  task_engine te;
  REQUIRE(te.ensure_task(simple_task("a", 4, log)));

  te.start_task("a", 2);  // run first two steps only
  te.wait_at("a", 2);
  CHECK(log.snapshot() == std::vector<std::string>{ "a:0", "a:1" });
  CHECK(te.completed("a") == 2);
  CHECK(te.target("a") == 2);

  te.extend_target("a", 1);  // downward is a no-op
  CHECK(te.target("a") == 2);

  te.extend_target("a", 4);  // resume to done
  te.wait_at("a", 4);
  CHECK(log.snapshot() == std::vector<std::string>{ "a:0", "a:1", "a:2", "a:3" });
}

TEST_CASE("task_engine: start_task reports thread creation exactly once") {
  step_log log;
  task_engine te;
  REQUIRE(te.ensure_task(simple_task("a", 2, log)));

  int before_spawn_calls{ 0 };
  CHECK(te.start_task("a", 1, [&] { ++before_spawn_calls; }));
  CHECK_FALSE(te.start_task("a", 2, [&] { ++before_spawn_calls; }));
  CHECK(before_spawn_calls == 1);
  te.wait_at("a", 2);  // second start extended the target
}

TEST_CASE("task_engine: edges sequence a chain") {
  step_log log;
  task_engine te;

  for (auto const *key : { "a", "b", "c" }) {
    auto cfg{ simple_task(key, 1, log) };
    std::string const k{ key };
    if (k != "a") {
      std::string const dep{ k == "b" ? "a" : "b" };
      cfg.edges = [dep](int) { return std::vector<task_engine::edge>{ { dep, 1 } }; };
    }
    REQUIRE(te.ensure_task(std::move(cfg)));
  }

  // Start in reverse order to prove edges, not start order, sequence the work.
  te.start_task("c", 1);
  te.start_task("b", 1);
  te.start_task("a", 1);
  te.wait_at("c", 1);

  CHECK(log.snapshot() == std::vector<std::string>{ "a:0", "b:0", "c:0" });
}

TEST_CASE("task_engine: diamond edges run source first and sink last") {
  step_log log;
  task_engine te;

  auto add{ [&](char const *key, std::vector<task_engine::edge> edges) {
    auto cfg{ simple_task(key, 1, log) };
    if (!edges.empty()) {
      cfg.edges = [edges = std::move(edges)](int) { return edges; };
    }
    REQUIRE(te.ensure_task(std::move(cfg)));
  } };

  // Edge targets must exist before a task that references them starts: create
  // everything, then start in sink-first order to prove edges do the sequencing.
  add("d", { { "b", 1 }, { "c", 1 } });
  add("b", { { "a", 1 } });
  add("c", { { "a", 1 } });
  add("a", {});
  for (auto const *key : { "d", "b", "c", "a" }) { te.start_task(key, 1); }

  te.wait_at("d", 1);

  CHECK(log.snapshot().size() == 4);
  CHECK(log.index_of("a:0") == 0);
  CHECK(log.index_of("d:0") == 3);
}

TEST_CASE("task_engine: edge extend_to ratchets past the wait watermark") {
  step_log log;
  task_engine te;

  REQUIRE(te.ensure_task(simple_task("dep", 3, log)));

  auto consumer{ simple_task("consumer", 1, log) };
  consumer.edges = [](int) {
    // Wait for two steps, but demand the third runs (concurrently).
    return std::vector<task_engine::edge>{ { "dep", 2, 3 } };
  };
  REQUIRE(te.ensure_task(std::move(consumer)));

  te.start_task("dep", 1);  // dep pauses after step 0 until the edge ratchets it
  te.start_task("consumer", 1);
  te.wait_at("consumer", 1);

  te.join_all();
  CHECK(te.completed("dep") == 3);  // extend_to ran dep to done without a wait
  CHECK(log.index_of("dep:1") < log.index_of("consumer:0"));
}

TEST_CASE("task_engine: failure propagates through waits with the real message") {
  task_engine te;

  task_engine::task_config bad;
  bad.key = "bad";
  bad.step_count = 1;
  bad.step = [](int) -> bool { throw std::runtime_error("deliberate step failure"); };
  REQUIRE(te.ensure_task(std::move(bad)));

  step_log log;
  auto dependent{ simple_task("dependent", 1, log) };
  dependent.edges = [](int) { return std::vector<task_engine::edge>{ { "bad", 1 } }; };
  REQUIRE(te.ensure_task(std::move(dependent)));

  te.start_task("bad", 1);
  te.start_task("dependent", 1);

  CHECK_THROWS_WITH(te.wait_at("bad", 1), "deliberate step failure");
  CHECK_THROWS_WITH(te.wait_at("dependent", 1), "deliberate step failure");

  te.join_all();
  CHECK(te.failed("bad"));
  CHECK(te.failed("dependent"));
  CHECK(log.snapshot().empty());  // dependent's step never ran

  auto const failures{ te.collect_failures() };
  CHECK(failures.size() == 2);
}

TEST_CASE("task_engine: on_start and on_failed hooks fire") {
  std::atomic_bool started{ false };
  std::atomic_bool failed_hook{ false };

  task_engine te;
  task_engine::task_config cfg;
  cfg.key = "t";
  cfg.step_count = 1;
  cfg.on_start = [&] {
    started = true;
    throw std::runtime_error("on_start failure");
  };
  cfg.step = [](int) { return false; };
  cfg.on_failed = [&] { failed_hook = true; };
  REQUIRE(te.ensure_task(std::move(cfg)));

  te.start_task("t", 1);
  CHECK_THROWS_WITH(te.wait_at("t", 1), "on_start failure");
  te.join_all();
  CHECK(started);
  CHECK(failed_hook);
}

TEST_CASE("task_engine: step can finish the task early") {
  step_log log;
  task_engine te;

  auto cfg{ simple_task("t", 10, log) };
  cfg.step = [&log](int step) {
    log.record("t:" + std::to_string(step));
    return step == 1;  // finish after the second step
  };
  REQUIRE(te.ensure_task(std::move(cfg)));

  te.start_task("t", 10);
  te.wait_at("t", 10);  // early completion satisfies the full watermark
  CHECK(log.snapshot() == std::vector<std::string>{ "t:0", "t:1" });
  CHECK(te.completed("t") == 10);
}

TEST_CASE("task_engine: tasks spawned from a running step are joined and waited") {
  step_log log;
  task_engine te;

  task_engine::task_config parent;
  parent.key = "parent";
  parent.step_count = 1;
  parent.step = [&](int) {
    // Plain throw instead of doctest asserts: this runs on a worker thread.
    for (auto const *child : { "child1", "child2" }) {
      if (!te.ensure_task(simple_task(child, 1, log))) {
        throw std::runtime_error("child task collision");
      }
      te.start_task(child, 1);
    }
    te.wait_at("child1", 1);
    te.wait_at("child2", 1);
    log.record("parent:0");
    return false;
  };
  REQUIRE(te.ensure_task(std::move(parent)));

  te.start_task("parent", 1);
  te.join_all();  // must reap children created after joining began

  auto const entries{ log.snapshot() };
  CHECK(entries.size() == 3);
  CHECK(entries.back() == "parent:0");
  CHECK_FALSE(te.failed("parent"));
}

TEST_CASE("task_engine: fail_all wakes paused workers and blocked waiters") {
  step_log log;
  task_engine te;
  REQUIRE(te.ensure_task(simple_task("stuck", 5, log)));
  te.start_task("stuck", 1);  // completes step 0, then pauses at target
  te.wait_at("stuck", 1);

  te.fail_all();
  CHECK_THROWS_AS(te.wait_at("stuck", 5), std::runtime_error);
  te.join_all();
  CHECK(te.failed("stuck"));
}

TEST_CASE("task_engine: extend_all_to_done finishes paused tasks") {
  step_log log;
  task_engine te;
  for (auto const *key : { "a", "b" }) {
    REQUIRE(te.ensure_task(simple_task(key, 3, log)));
    te.start_task(key, 1);
  }
  te.wait_at("a", 1);
  te.wait_at("b", 1);

  te.extend_all_to_done();
  te.join_all();
  CHECK(te.completed("a") == 3);
  CHECK(te.completed("b") == 3);
}

TEST_CASE("task_engine: global condition rendezvous") {
  std::atomic_int counter{ 2 };
  task_engine te;

  for (auto const *key : { "a", "b" }) {
    task_engine::task_config cfg;
    cfg.key = key;
    cfg.step_count = 1;
    cfg.step = [&te, &counter](int) {
      --counter;
      te.notify_global();
      return false;
    };
    REQUIRE(te.ensure_task(std::move(cfg)));
    te.start_task(key, 1);
  }

  te.wait_global([&] { return counter == 0; });
  CHECK(counter == 0);
}

TEST_CASE("task_engine: observer sees lifecycle events") {
  std::atomic_int blocked{ 0 };
  std::atomic_int unblocked{ 0 };
  std::atomic_int extended{ 0 };

  task_engine::observer obs;
  obs.blocked = [&](std::string const &, int, std::string const &, int) { ++blocked; };
  obs.unblocked = [&](std::string const &, int, std::string const &) { ++unblocked; };
  obs.target_extended = [&](std::string const &, int, int) { ++extended; };

  step_log log;
  task_engine te{ std::move(obs) };

  REQUIRE(te.ensure_task(simple_task("a", 2, log)));
  auto b{ simple_task("b", 1, log) };
  b.edges = [](int) { return std::vector<task_engine::edge>{ { "a", 2 } }; };
  REQUIRE(te.ensure_task(std::move(b)));

  te.start_task("a", 1);
  te.wait_at("a", 1);  // a parks at 1; b's edge wants 2, so b must really block
  te.start_task("b", 1);
  te.wait_at("b", 1);  // b's own wait ratchets a to 2 -> target_extended fires
  te.join_all();

  CHECK(blocked == 1);
  CHECK(unblocked == 1);
  CHECK(extended >= 1);
}

TEST_CASE("task_engine: stress - layered graph completes every task") {
  // 8 layers x 8 tasks; each task depends on every task in the previous layer.
  constexpr int kLayers{ 8 };
  constexpr int kWidth{ 8 };

  std::atomic_int steps_run{ 0 };
  task_engine te;

  auto key_of{ [](int layer, int i) {
    return "t" + std::to_string(layer) + "_" + std::to_string(i);
  } };

  for (int layer{ 0 }; layer < kLayers; ++layer) {
    std::vector<task_engine::edge> edges;
    if (layer > 0) {
      for (int i{ 0 }; i < kWidth; ++i) { edges.push_back({ key_of(layer - 1, i), 2 }); }
    }
    for (int i{ 0 }; i < kWidth; ++i) {
      task_engine::task_config cfg;
      cfg.key = key_of(layer, i);
      cfg.step_count = 2;
      cfg.step = [&steps_run](int) {
        ++steps_run;
        return false;
      };
      if (!edges.empty()) {
        cfg.edges = [edges](int step) {
          return step == 0 ? edges : std::vector<task_engine::edge>{};
        };
      }
      REQUIRE(te.ensure_task(std::move(cfg)));
    }
  }

  // Start sinks first; edge waits ratchet predecessors' targets on demand.
  for (int layer{ kLayers - 1 }; layer >= 0; --layer) {
    for (int i{ 0 }; i < kWidth; ++i) { te.start_task(key_of(layer, i), 2); }
  }

  for (int i{ 0 }; i < kWidth; ++i) { te.wait_at(key_of(kLayers - 1, i), 2); }
  te.join_all();

  CHECK(steps_run == kLayers * kWidth * 2);
  CHECK(te.collect_failures().empty());
}

// ============================================================================
// Race stress tests. Assertions stay on the main thread; workers and helper
// threads communicate through atomics and throw plain exceptions. Iteration
// counts are tuned to shake interleavings while keeping each case fast.
// ============================================================================

TEST_CASE("task_engine: race - concurrent start_task creates exactly one worker") {
  constexpr int kIterations{ 50 };
  constexpr int kThreads{ 8 };

  for (int iter{ 0 }; iter < kIterations; ++iter) {
    task_engine te;
    std::atomic_int step_runs{ 0 };
    std::atomic_int before_spawns{ 0 };
    std::atomic_int created{ 0 };

    task_engine::task_config cfg;
    cfg.key = "t";
    cfg.step_count = 1;
    cfg.step = [&](int) {
      ++step_runs;
      return false;
    };
    REQUIRE(te.ensure_task(std::move(cfg)));

    std::atomic_int gate{ 0 };  // spin barrier: maximize start_task overlap
    std::vector<std::thread> starters;
    starters.reserve(kThreads);
    for (int i{ 0 }; i < kThreads; ++i) {
      starters.emplace_back([&] {
        ++gate;
        while (gate < kThreads) { std::this_thread::yield(); }
        if (te.start_task("t", 1, [&] { ++before_spawns; })) { ++created; }
      });
    }
    for (auto &s : starters) { s.join(); }
    te.join_all();

    CHECK(created == 1);
    CHECK(before_spawns == 1);
    CHECK(step_runs == 1);
    CHECK(te.completed("t") == 1);
  }
}

TEST_CASE("task_engine: race - concurrent ensure_task interns exactly once") {
  constexpr int kIterations{ 50 };
  constexpr int kThreads{ 8 };

  for (int iter{ 0 }; iter < kIterations; ++iter) {
    task_engine te;
    std::atomic_int wins{ 0 };
    std::atomic_int gate{ 0 };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i{ 0 }; i < kThreads; ++i) {
      threads.emplace_back([&, i] {
        task_engine::task_config cfg;
        cfg.key = "t";
        cfg.step_count = i + 1;  // distinct configs: winner's must survive
        cfg.step = [](int) { return false; };
        ++gate;
        while (gate < kThreads) { std::this_thread::yield(); }
        if (te.ensure_task(std::move(cfg))) { ++wins; }
      });
    }
    for (auto &t : threads) { t.join(); }

    CHECK(wins == 1);
    CHECK(te.contains("t"));
    CHECK(te.step_count("t") >= 1);
  }
}

TEST_CASE("task_engine: race - target ratchet hammer never loses a wakeup") {
  // A worker repeatedly parks at its target while an external thread ratchets
  // the target upward one step at a time as fast as possible — the tightest
  // window for the classic lost-wakeup bug.
  constexpr int kSteps{ 512 };

  task_engine te;
  std::atomic_int runs{ 0 };
  task_engine::task_config cfg;
  cfg.key = "t";
  cfg.step_count = kSteps;
  cfg.step = [&](int) {
    ++runs;
    return false;
  };
  REQUIRE(te.ensure_task(std::move(cfg)));
  te.start_task("t", 1);

  std::thread ratchet([&] {
    for (int w{ 2 }; w <= kSteps; ++w) { te.extend_target("t", w); }
  });
  ratchet.join();
  te.wait_at("t", kSteps);
  te.join_all();

  CHECK(runs == kSteps);
  CHECK(te.completed("t") == kSteps);
}

TEST_CASE("task_engine: race - concurrent ratchets from many threads") {
  constexpr int kIterations{ 20 };
  constexpr int kSteps{ 128 };
  constexpr int kThreads{ 4 };

  for (int iter{ 0 }; iter < kIterations; ++iter) {
    task_engine te;
    REQUIRE(te.ensure_task([&] {
      task_engine::task_config cfg;
      cfg.key = "t";
      cfg.step_count = kSteps;
      cfg.step = [](int) { return false; };
      return cfg;
    }()));
    te.start_task("t", 1);

    std::vector<std::thread> ratchets;
    ratchets.reserve(kThreads);
    for (int i{ 0 }; i < kThreads; ++i) {
      ratchets.emplace_back([&, i] {
        // Interleaved strides; collectively they cover every watermark.
        for (int w{ i + 1 }; w <= kSteps; w += kThreads) { te.extend_target("t", w); }
        te.extend_target("t", kSteps);
      });
    }
    for (auto &r : ratchets) { r.join(); }
    te.wait_at("t", kSteps);
    te.join_all();
    CHECK(te.completed("t") == kSteps);
  }
}

TEST_CASE("task_engine: race - waiters racing completion never hang") {
  constexpr int kTasks{ 16 };
  constexpr int kWaiters{ 8 };

  task_engine te;
  for (int i{ 0 }; i < kTasks; ++i) {
    task_engine::task_config cfg;
    cfg.key = "t" + std::to_string(i);
    cfg.step_count = 4;
    cfg.step = [](int) { return false; };
    REQUIRE(te.ensure_task(std::move(cfg)));
  }

  std::vector<std::thread> waiters;
  waiters.reserve(kWaiters);
  std::atomic_int satisfied{ 0 };
  for (int w{ 0 }; w < kWaiters; ++w) {
    waiters.emplace_back([&, w] {
      // Each waiter sweeps all tasks at varied watermarks, racing execution.
      for (int i{ 0 }; i < kTasks; ++i) {
        te.wait_at("t" + std::to_string(i), 1 + (w + i) % 4);
        ++satisfied;
      }
    });
  }
  for (int i{ 0 }; i < kTasks; ++i) { te.start_task("t" + std::to_string(i), 4); }
  for (auto &w : waiters) { w.join(); }
  te.join_all();

  CHECK(satisfied == kTasks * kWaiters);
  CHECK(te.collect_failures().empty());
}

TEST_CASE("task_engine: race - waiters racing failure all observe it") {
  constexpr int kIterations{ 25 };
  constexpr int kWaiters{ 4 };

  for (int iter{ 0 }; iter < kIterations; ++iter) {
    task_engine te;
    task_engine::task_config cfg;
    cfg.key = "bad";
    cfg.step_count = 2;
    cfg.step = [](int step) -> bool {
      if (step == 1) { throw std::runtime_error("boom"); }
      return false;
    };
    REQUIRE(te.ensure_task(std::move(cfg)));

    std::atomic_int threw{ 0 };
    std::vector<std::thread> waiters;
    waiters.reserve(kWaiters);
    for (int w{ 0 }; w < kWaiters; ++w) {
      waiters.emplace_back([&] {
        try {
          te.wait_at("bad", 2);
        } catch (std::runtime_error const &) { ++threw; }
      });
    }
    te.start_task("bad", 2);
    for (auto &w : waiters) { w.join(); }
    te.join_all();

    CHECK(threw == kWaiters);
    CHECK_THROWS_WITH(te.wait_at("bad", 2), "boom");  // late waiter too
  }
}

TEST_CASE("task_engine: race - fail_all teardown at random points terminates") {
  constexpr int kIterations{ 20 };
  constexpr int kTasks{ 24 };
  std::mt19937 rng{ 0xC0FFEE };

  for (int iter{ 0 }; iter < kIterations; ++iter) {
    task_engine te;
    std::atomic_int running{ 0 };

    for (int i{ 0 }; i < kTasks; ++i) {
      task_engine::task_config cfg;
      cfg.key = "t" + std::to_string(i);
      cfg.step_count = 8;
      cfg.step = [&running](int) {
        ++running;
        std::this_thread::yield();
        return false;
      };
      if (i > 0) {  // sparse chain edges to mix blocked and running workers
        cfg.edges = [dep = "t" + std::to_string(i / 2)](int step) {
          return step == 0 ? std::vector<task_engine::edge>{ { dep, 4 } }
                           : std::vector<task_engine::edge>{};
        };
      }
      REQUIRE(te.ensure_task(std::move(cfg)));
    }
    for (int i{ 0 }; i < kTasks; ++i) { te.start_task("t" + std::to_string(i), 8); }

    // Tear down after a random slice of progress.
    int const threshold{ static_cast<int>(rng() % (kTasks * 4)) };
    while (running < threshold) { std::this_thread::yield(); }
    te.fail_all();
    te.join_all();  // must return: no worker may sleep through the teardown

    for (int i{ 0 }; i < kTasks; ++i) {  // every task terminal: failed or done
      std::string const key{ "t" + std::to_string(i) };
      CHECK((te.failed(key) || te.completed(key) == 8));
    }
  }
}

TEST_CASE("task_engine: race - recursive spawn tree reaped by concurrent join") {
  // Root spawns children which spawn grandchildren, while the main thread is
  // already inside join_all — the rescan must reap every generation.
  constexpr int kDepth{ 4 };

  task_engine te;
  std::atomic_int leaf_runs{ 0 };

  std::function<task_engine::task_config(std::string, int)> make_node{ [&](std::string key,
                                                                           int depth) {
    task_engine::task_config cfg;
    cfg.key = key;
    cfg.step_count = 1;
    cfg.step = [&, key, depth](int) {
      if (depth == kDepth) {
        ++leaf_runs;
        return false;
      }
      for (int c{ 0 }; c < 2; ++c) {
        std::string const child{ key + "." + std::to_string(c) };
        if (!te.ensure_task(make_node(child, depth + 1))) {
          throw std::runtime_error("collision: " + child);
        }
        te.start_task(child, 1);
      }
      for (int c{ 0 }; c < 2; ++c) { te.wait_at(key + "." + std::to_string(c), 1); }
      return false;
    };
    return cfg;
  } };

  REQUIRE(te.ensure_task(make_node("root", 1)));
  te.start_task("root", 1);
  te.join_all();

  CHECK(leaf_runs == (1 << (kDepth - 1)));
  CHECK(te.collect_failures().empty());
  CHECK(te.completed("root") == 1);
}

TEST_CASE("task_engine: race - random DAG respects every edge under load") {
  // Layered random graph; each step records a global sequence tick at entry
  // and exit. Post-join, every edge's dependency must have finished its
  // watermark strictly before the dependent's gated step began.
  constexpr int kLayers{ 6 };
  constexpr int kWidth{ 12 };
  constexpr int kSteps{ 2 };
  std::mt19937 rng{ 0xDECAF };

  task_engine te;
  std::atomic_int tick{ 0 };

  struct node_times {
    std::atomic_int step0_start{ -1 };
    std::atomic_int done{ -1 };
  };
  std::vector<std::vector<node_times>> times(kLayers);
  for (auto &layer : times) { layer = std::vector<node_times>(kWidth); }

  std::vector<std::vector<std::vector<std::pair<int, int>>>> deps(kLayers);
  auto key_of{ [](int layer, int i) {
    return "n" + std::to_string(layer) + "_" + std::to_string(i);
  } };

  for (int layer{ 0 }; layer < kLayers; ++layer) {
    deps[layer].resize(kWidth);
    for (int i{ 0 }; i < kWidth; ++i) {
      if (layer > 0) {  // 1-3 random deps from any earlier layer
        int const fan{ 1 + static_cast<int>(rng() % 3) };
        for (int d{ 0 }; d < fan; ++d) {
          int const dl{ static_cast<int>(rng() % static_cast<unsigned>(layer)) };
          deps[layer][i].emplace_back(dl, static_cast<int>(rng() % kWidth));
        }
      }

      task_engine::task_config cfg;
      cfg.key = key_of(layer, i);
      cfg.step_count = kSteps;
      cfg.step = [&, layer, i](int step) {
        if (step == 0) { times[layer][i].step0_start = tick++; }
        std::this_thread::yield();
        if (step == kSteps - 1) { times[layer][i].done = tick++; }
        return false;
      };
      if (!deps[layer][i].empty()) {
        std::vector<task_engine::edge> edges;
        for (auto const &[dl, di] : deps[layer][i]) {
          edges.push_back({ key_of(dl, di), kSteps });
        }
        cfg.edges = [edges = std::move(edges)](int step) {
          return step == 0 ? edges : std::vector<task_engine::edge>{};
        };
      }
      REQUIRE(te.ensure_task(std::move(cfg)));
    }
  }

  for (int layer{ kLayers - 1 }; layer >= 0; --layer) {  // start sinks first
    for (int i{ 0 }; i < kWidth; ++i) { te.start_task(key_of(layer, i), kSteps); }
  }
  te.join_all();

  REQUIRE(te.collect_failures().empty());
  for (int layer{ 1 }; layer < kLayers; ++layer) {
    for (int i{ 0 }; i < kWidth; ++i) {
      for (auto const &[dl, di] : deps[layer][i]) {
        CHECK(times[dl][di].done < times[layer][i].step0_start);
      }
    }
  }
}

TEST_CASE("task_engine: race - global rendezvous hammer") {
  constexpr int kIterations{ 25 };
  constexpr int kTasks{ 16 };

  for (int iter{ 0 }; iter < kIterations; ++iter) {
    task_engine te;
    std::atomic_int countdown{ kTasks };

    for (int i{ 0 }; i < kTasks; ++i) {
      task_engine::task_config cfg;
      cfg.key = "t" + std::to_string(i);
      cfg.step_count = 1;
      cfg.step = [&](int) {
        --countdown;
        te.notify_global();
        return false;
      };
      REQUIRE(te.ensure_task(std::move(cfg)));
      te.start_task("t" + std::to_string(i), 1);
    }

    te.wait_global([&] { return countdown == 0; });
    te.join_all();
    CHECK(countdown == 0);
  }
}

TEST_CASE("task_engine: race - early completion releases high-watermark waiters") {
  constexpr int kIterations{ 25 };
  constexpr int kWaiters{ 4 };

  for (int iter{ 0 }; iter < kIterations; ++iter) {
    task_engine te;
    task_engine::task_config cfg;
    cfg.key = "t";
    cfg.step_count = 100;
    cfg.step = [](int) { return true; };  // finish on the first step
    REQUIRE(te.ensure_task(std::move(cfg)));

    std::vector<std::thread> waiters;
    waiters.reserve(kWaiters);
    for (int w{ 0 }; w < kWaiters; ++w) {
      waiters.emplace_back([&] { te.wait_at("t", 100); });
    }
    te.start_task("t", 100);
    for (auto &w : waiters) { w.join(); }
    te.join_all();
    CHECK(te.completed("t") == 100);
  }
}

TEST_CASE("task_engine: race - extend_all_to_done racing task creation") {
  constexpr int kTasks{ 64 };

  task_engine te;
  std::atomic_bool creating{ true };

  std::thread creator([&] {
    for (int i{ 0 }; i < kTasks; ++i) {
      task_engine::task_config cfg;
      cfg.key = "t" + std::to_string(i);
      cfg.step_count = 3;
      cfg.step = [](int) { return false; };
      te.ensure_task(std::move(cfg));
      te.start_task("t" + std::to_string(i), 1);  // partial target
    }
    creating = false;
  });
  std::thread extender([&] {
    while (creating) {
      te.extend_all_to_done();
      std::this_thread::yield();
    }
  });

  creator.join();
  extender.join();
  te.extend_all_to_done();  // cover tasks created after the extender's last pass
  te.join_all();

  for (int i{ 0 }; i < kTasks; ++i) { CHECK(te.completed("t" + std::to_string(i)) == 3); }
}

TEST_CASE("task_engine: on_failed fires exactly once per failure mode") {
  auto run_mode{ [](char const *mode) {
    task_engine te;
    std::atomic_int hook_calls{ 0 };

    if (std::string_view{ mode } == "edge-wait") {
      task_engine::task_config bad;
      bad.key = "bad";
      bad.step_count = 1;
      bad.step = [](int) -> bool { throw std::runtime_error("dep boom"); };
      REQUIRE(te.ensure_task(std::move(bad)));
      te.start_task("bad", 1);
    }

    task_engine::task_config cfg;
    cfg.key = "t";
    cfg.step_count = 1;
    cfg.on_failed = [&] { ++hook_calls; };
    if (std::string_view{ mode } == "step") {
      cfg.step = [](int) -> bool { throw std::runtime_error("step boom"); };
    } else if (std::string_view{ mode } == "on_start") {
      cfg.step = [](int) { return false; };
      cfg.on_start = [] { throw std::runtime_error("start boom"); };
    } else {  // edge-wait: waiting on the failed dep throws inside the worker
      cfg.step = [](int) { return false; };
      cfg.edges = [](int) { return std::vector<task_engine::edge>{ { "bad", 1 } }; };
    }
    REQUIRE(te.ensure_task(std::move(cfg)));
    te.start_task("t", 1);
    te.join_all();

    CHECK(te.failed("t"));
    CHECK(hook_calls == 1);
  } };

  run_mode("step");
  run_mode("on_start");
  run_mode("edge-wait");
}

TEST_CASE("task_engine: before_spawn throw fails the task instead of stranding it") {
  task_engine te;
  task_engine::task_config cfg;
  cfg.key = "t";
  cfg.step_count = 1;
  cfg.step = [](int) { return false; };
  REQUIRE(te.ensure_task(std::move(cfg)));

  CHECK_THROWS_WITH(te.start_task("t", 1, [] { throw std::runtime_error("spawn boom"); }),
                    "spawn boom");

  // Waiters must observe the failure rather than hang on a task that can
  // never run (started is latched, no worker exists).
  CHECK(te.failed("t"));
  CHECK_THROWS_WITH(te.wait_at("t", 1), "spawn boom");
  te.join_all();
  auto const failures{ te.collect_failures() };
  REQUIRE(failures.size() == 1);
  CHECK(failures.front().second == "spawn boom");
}

TEST_CASE("task_engine: edges callback throw fails the task") {
  task_engine te;
  task_engine::task_config cfg;
  cfg.key = "t";
  cfg.step_count = 1;
  cfg.step = [](int) { return false; };
  cfg.edges = [](int) -> std::vector<task_engine::edge> {
    throw std::runtime_error("edges boom");
  };
  REQUIRE(te.ensure_task(std::move(cfg)));
  te.start_task("t", 1);
  CHECK_THROWS_WITH(te.wait_at("t", 1), "edges boom");
  te.join_all();
}

TEST_CASE("task_engine: observer callbacks may reenter engine queries") {
  // The engine must invoke observers with no locks held; prove it by querying
  // back into the engine from every callback while a small graph runs.
  std::atomic_int reentrant_queries{ 0 };
  task_engine *te_ptr{ nullptr };

  task_engine::observer obs;
  auto query{ [&](std::string const &key) {
    if (te_ptr && te_ptr->contains(key)) {
      te_ptr->completed(key);
      te_ptr->target(key);
      te_ptr->failed(key);
      ++reentrant_queries;
    }
  } };
  obs.blocked = [&](std::string const &key, int, std::string const &, int) { query(key); };
  obs.unblocked = [&](std::string const &key, int, std::string const &) { query(key); };
  obs.target_extended = [&](std::string const &key, int, int) { query(key); };

  task_engine te{ std::move(obs) };
  te_ptr = &te;

  step_log log;
  REQUIRE(te.ensure_task(simple_task("a", 2, log)));
  auto b{ simple_task("b", 1, log) };
  b.edges = [](int) { return std::vector<task_engine::edge>{ { "a", 2 } }; };
  REQUIRE(te.ensure_task(std::move(b)));

  te.start_task("a", 1);
  te.wait_at("a", 1);
  te.extend_target("a", 2);
  te.start_task("b", 1);
  te.wait_at("b", 1);
  te.join_all();

  CHECK(reentrant_queries > 0);
  CHECK(te.collect_failures().empty());
}

TEST_CASE("task_engine: oversized watermarks clamp to done instead of hanging") {
  step_log log;
  task_engine te;
  REQUIRE(te.ensure_task(simple_task("t", 2, log)));

  te.extend_target("t", 100);
  CHECK(te.target("t") == 2);  // ratchet clamps: beyond-done is unsatisfiable

  te.start_task("t", 500);
  te.wait_at("t", 1000);  // waits for done rather than an impossible watermark
  te.join_all();
  CHECK(te.completed("t") == 2);
}

TEST_CASE("task_engine: zero-step task, trivial watermarks, repeated joins") {
  task_engine te;
  task_engine::task_config cfg;
  cfg.key = "empty";
  cfg.step_count = 0;
  REQUIRE(te.ensure_task(std::move(cfg)));

  te.wait_at("empty", 0);  // trivially satisfied before start
  te.start_task("empty", 0);
  te.wait_at("empty", 0);
  te.join_all();
  te.join_all();  // idempotent
  CHECK(te.completed("empty") == 0);
  CHECK_FALSE(te.failed("empty"));
}

TEST_CASE("task_engine: stress - mid-graph failure fails all dependents") {
  // Chain of 20 tasks; task 7 fails. Everything downstream fails with its
  // message; everything upstream completes.
  constexpr int kCount{ 20 };
  constexpr int kFailing{ 7 };

  task_engine te;
  auto key_of{ [](int i) { return "n" + std::to_string(i); } };

  for (int i{ 0 }; i < kCount; ++i) {
    task_engine::task_config cfg;
    cfg.key = key_of(i);
    cfg.step_count = 1;
    cfg.step = [i](int) -> bool {
      if (i == kFailing) { throw std::runtime_error("n7 deliberate failure"); }
      return false;
    };
    if (i > 0) {
      cfg.edges = [dep = key_of(i - 1)](int) {
        return std::vector<task_engine::edge>{ { dep, 1 } };
      };
    }
    REQUIRE(te.ensure_task(std::move(cfg)));
  }

  for (int i{ 0 }; i < kCount; ++i) { te.start_task(key_of(i), 1); }
  te.join_all();

  for (int i{ 0 }; i < kCount; ++i) {
    if (i < kFailing) {
      CHECK_FALSE(te.failed(key_of(i)));
      CHECK(te.completed(key_of(i)) == 1);
    } else {
      CHECK(te.failed(key_of(i)));
      CHECK_THROWS_WITH(te.wait_at(key_of(i), 1), "n7 deliberate failure");
    }
  }
}

TEST_CASE("task_engine: extend_all_to_done latches for later tasks") {
  step_log log;
  task_engine te;
  te.extend_all_to_done();  // latches: every task from here on runs to done

  REQUIRE(te.ensure_task(simple_task("late", 3, log)));
  te.start_task("late", 1);  // partial target: without the latch it parks at 1 forever

  CHECK(spin_until([&te] { return te.completed("late") == 3; }));
  te.extend_all_to_done();  // unwedges the parked worker if the latch regressed
  te.join_all();
  CHECK(te.completed("late") == 3);
}

TEST_CASE("task_engine: the run-to-done latch ends with join_all") {
  step_log log;
  task_engine te;
  REQUIRE(te.ensure_task(simple_task("a", 2, log)));
  te.start_task("a", 2);
  te.extend_all_to_done();
  te.join_all();  // teardown over: a later wave targets what its caller asked for

  REQUIRE(te.ensure_task(simple_task("b", 3, log)));
  te.start_task("b", 1);
  te.wait_at("b", 1);
  CHECK(te.completed("b") == 1);
  CHECK(te.target("b") == 1);
}

TEST_CASE("task_engine: fail_all fires on_failed for a parked worker") {
  step_log log;
  std::atomic_int hook_calls{ 0 };

  task_engine te;
  auto cfg{ simple_task("stuck", 5, log) };
  cfg.on_failed = [&hook_calls] { ++hook_calls; };
  REQUIRE(te.ensure_task(std::move(cfg)));

  te.start_task("stuck", 1);
  te.wait_at("stuck", 1);  // parked at its target, mid-task

  te.fail_all();  // real cancellation: the worker owes its failure hook
  te.join_all();
  CHECK(hook_calls == 1);
  CHECK(te.failed("stuck"));
}

TEST_CASE("task_engine: tasks interned after fail_all are born failed") {
  task_engine te;
  te.fail_all();

  std::atomic_int starts{ 0 };
  std::atomic_int steps{ 0 };
  std::atomic_int hook_calls{ 0 };

  task_engine::task_config cfg;
  cfg.key = "late";
  cfg.step_count = 1;
  cfg.on_start = [&starts] { ++starts; };
  cfg.step = [&steps](int) {
    ++steps;
    return false;
  };
  cfg.on_failed = [&hook_calls] { ++hook_calls; };
  REQUIRE(te.ensure_task(std::move(cfg)));
  CHECK(te.failed("late"));

  te.start_task("late", 1);
  te.join_all();
  CHECK(starts == 0);  // nothing spawned mid-teardown may run host-side work
  CHECK(steps == 0);
  CHECK(hook_calls == 1);
  CHECK_THROWS_AS(te.wait_at("late", 1), std::runtime_error);
}

TEST_CASE("task_engine: a satisfied watermark survives the dep's later failure") {
  step_log log;
  task_engine te;

  task_engine::task_config dep;
  dep.key = "dep";
  dep.step_count = 2;
  dep.step = [](int step) -> bool {
    if (step == 1) { throw std::runtime_error("late boom"); }
    return false;
  };
  REQUIRE(te.ensure_task(std::move(dep)));

  auto consumer{ simple_task("consumer", 1, log) };
  consumer.edges = [](int) { return std::vector<task_engine::edge>{ { "dep", 1 } }; };
  REQUIRE(te.ensure_task(std::move(consumer)));

  te.start_task("dep", 2);
  REQUIRE(spin_until([&te] { return te.failed("dep"); }));
  REQUIRE(te.completed("dep") == 1);  // step 0 landed before step 1 threw

  CHECK_NOTHROW(te.wait_at("dep", 1));  // the watermark it needed is satisfied
  te.start_task("consumer", 1);
  CHECK(spin_until([&te] { return te.completed("consumer") == 1; }));
  CHECK_FALSE(te.failed("consumer"));
  te.join_all();
}

TEST_CASE("task_engine: observer stays silent on already-satisfied edges") {
  std::atomic_int blocked{ 0 };
  std::atomic_int unblocked{ 0 };

  task_engine::observer obs;
  obs.blocked = [&blocked](std::string const &, int, std::string const &, int) {
    ++blocked;
  };
  obs.unblocked = [&unblocked](std::string const &, int, std::string const &) {
    ++unblocked;
  };

  step_log log;
  task_engine te{ std::move(obs) };
  REQUIRE(te.ensure_task(simple_task("a", 1, log)));
  auto b{ simple_task("b", 1, log) };
  b.edges = [](int) { return std::vector<task_engine::edge>{ { "a", 1 } }; };
  REQUIRE(te.ensure_task(std::move(b)));

  te.start_task("a", 1);
  te.wait_at("a", 1);  // a is done before b's worker ever queries its edges
  te.start_task("b", 1);
  te.wait_at("b", 1);
  te.join_all();

  CHECK(blocked == 0);
  CHECK(unblocked == 0);
}

TEST_CASE("task_engine: worker spawn failure settles the task instead of spinning") {
  // Heap-allocated, deleted only on success: if this regresses, join_all
  // rescans forever and the joiner below must not outlive the engine.
  auto *te{ new task_engine{} };
  task_engine::task_config cfg;
  cfg.key = "t";
  cfg.step_count = 1;
  cfg.step = [](int) { return false; };
  REQUIRE(te->ensure_task(std::move(cfg)));

  te->spawn_failure_hook = [] { throw std::runtime_error("spawn boom"); };
  CHECK_THROWS_WITH(te->start_task("t", 1), "spawn boom");
  CHECK(te->failed("t"));  // waiters must see an error, not a task that can never run

  std::atomic_bool joined{ false };
  std::thread joiner{ [te, &joined] {
    te->join_all();
    joined = true;
  } };
  bool const settled{ spin_until([&joined] { return joined.load(); }) };
  CHECK(settled);  // a started task whose spawn never settles spins join_all
  if (!settled) {
    joiner.detach();  // engine deliberately leaked: the spin still touches it
    return;
  }
  joiner.join();
  auto const failures{ te->collect_failures() };
  REQUIRE(failures.size() == 1);
  CHECK(failures.front().second == "spawn boom");
  delete te;
}

TEST_CASE("task_engine: watchdog sees a wait nested inside a step") {
  // a's step waits on b while b's edge waits on a. Both threads are blocked
  // inside the engine, so neither may count as running. Heap-allocated,
  // deleted only on success (see above).
  auto *te{ new task_engine{} };
  te->set_watchdog_interval(std::chrono::milliseconds{ 25 });

  task_engine::task_config a;
  a.key = "a";
  a.step_count = 1;
  a.step = [te](int) {
    te->wait_at("b", 1);  // blocked inside a step: waiting, not running
    return false;
  };
  REQUIRE(te->ensure_task(std::move(a)));

  task_engine::task_config b;
  b.key = "b";
  b.step_count = 1;
  b.step = [](int) { return false; };
  b.edges = [](int) { return std::vector<task_engine::edge>{ { "a", 1 } }; };
  REQUIRE(te->ensure_task(std::move(b)));

  te->start_task("a", 1);
  te->start_task("b", 1);

  bool const fired{ spin_until([te] { return te->failed("a") && te->failed("b"); }) };
  CHECK(fired);  // an in-step wait must not suppress the watchdog
  if (!fired) { return; }

  te->join_all();
  auto const failures{ te->collect_failures() };
  REQUIRE(failures.size() == 2);
  for (auto const &[key, msg] : failures) {
    CHECK(msg.find("a step 0 waits for b@1") != std::string::npos);
    CHECK(msg.find("b step 0 waits for a@1") != std::string::npos);
  }
  delete te;
}

TEST_CASE("task_engine: watchdog turns an edge cycle into a failure") {
  // a and b each block on the other's first step: nothing runs, everything
  // waits. Heap-allocated, deleted only on success (see above).
  auto *te{ new task_engine{} };
  te->set_watchdog_interval(std::chrono::milliseconds{ 25 });

  for (auto const *key : { "a", "b" }) {
    task_engine::task_config cfg;
    cfg.key = key;
    cfg.step_count = 1;
    cfg.step = [](int) { return false; };
    cfg.edges = [dep = std::string{ key } == "a" ? "b" : "a"](int) {
      return std::vector<task_engine::edge>{ { dep, 1 } };
    };
    REQUIRE(te->ensure_task(std::move(cfg)));
  }
  te->start_task("a", 1);
  te->start_task("b", 1);

  std::atomic_bool done{ false };
  std::string message;
  std::thread waiter{ [te, &done, &message] {
    try {
      te->wait_at("a", 1);
    } catch (std::exception const &e) { message = e.what(); }
    done = true;
  } };

  bool const fired{ spin_until([&done] { return done.load(); }) };
  CHECK(fired);  // without the watchdog the cycle simply hangs
  if (!fired) {
    waiter.detach();
    return;
  }
  waiter.join();
  CHECK(message.find("Deadlock") != std::string::npos);
  CHECK(message.find("a step 0") != std::string::npos);
  CHECK(message.find("b step 0") != std::string::npos);

  te->join_all();
  CHECK(te->failed("a"));
  CHECK(te->failed("b"));
  delete te;
}

}  // namespace envy
