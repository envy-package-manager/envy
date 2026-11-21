#pragma once

#include "util.h"

#include <condition_variable>
#include <exception>
#include <mutex>

namespace envy {

// Carries a C library's callback throws across its C frames: guard() keeps the first and
// runs on_throw (cancel); wait() rethrows it once finish(), the last call, has arrived.
class c_completion : unmovable {
 public:
  template <typename F, typename G>
  void guard(F &&body, G &&on_throw) noexcept;
  void finish() noexcept;
  void wait();

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool done_{ false };
  std::exception_ptr error_;
};

template <typename F, typename G>
void c_completion::guard(F &&body, G &&on_throw) noexcept {
  if (std::lock_guard const lock{ mutex_ }; error_) { return; }  // already failing
  try {
    body();
  } catch (...) {
    {
      std::lock_guard const lock{ mutex_ };
      if (!error_) { error_ = std::current_exception(); }
    }
    on_throw();
  }
}

inline void c_completion::finish() noexcept {  // Notifies under the lock: wait()'s
  std::lock_guard const lock{ mutex_ };         // caller may free *this on return.
  done_ = true;
  cv_.notify_all();
}

inline void c_completion::wait() {
  std::unique_lock lock{ mutex_ };
  cv_.wait(lock, [this] { return done_; });
  if (error_) { std::rethrow_exception(error_); }
}

}  // namespace envy
