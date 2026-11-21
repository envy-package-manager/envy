#include "c_completion.h"

#include "doctest.h"

#include <stdexcept>
#include <thread>

TEST_CASE("c_completion: a callback's throw is rethrown on the waiting thread") {
  envy::c_completion done;
  bool cancelled{ false };
  int later_bodies{ 0 };
  std::thread library{ [&] {  // a C library's event-loop thread
    done.guard([] { throw std::runtime_error("from a callback"); },
               [&] { cancelled = true; });
    done.guard([&] { ++later_bodies; }, [] {});  // a failed transfer runs no more bodies
    done.finish();
  } };
  CHECK_THROWS_WITH_AS(done.wait(), "from a callback", std::runtime_error);
  library.join();
  CHECK(cancelled);
  CHECK(later_bodies == 0);
}

TEST_CASE("c_completion: wait returns once finished when nothing threw") {
  envy::c_completion done;
  int ran{ 0 };
  bool cancelled{ false };
  std::thread library{ [&] {
    done.guard([&] { ++ran; }, [&] { cancelled = true; });
    done.finish();
  } };
  CHECK_NOTHROW(done.wait());
  library.join();
  CHECK(ran == 1);
  CHECK_FALSE(cancelled);
}
