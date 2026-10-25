#include "semver.h"

namespace envy {

namespace {

thread_local std::string *t_why{ nullptr };  // where this thread's next diagnostic goes

void append_why(void * /*ctx*/, char const *s, size_t len) {
  if (t_why) { t_why->append(s, len); }
}

// Points the hook at why for one call. The static's initialization is thread-safe and
// happens-before every later call, so no thread parses until the hook is set.
class why_scope {
 public:
  explicit why_scope(std::string *why) noexcept {
    static bool const installed{ (nsv_diag_set_print(&append_why, nullptr), true) };
    (void)installed;
    if (why) {
      why->assign(1, '\n');
      t_why = why;
    }
  }
  ~why_scope() {
    if (!t_why) { return; }
    if (t_why->size() == 1) {
      t_why->clear();  // no failure, or one nanosemver does not explain (ERR_ARG)
    } else if (t_why->back() == '\n') {
      t_why->pop_back();
    }
    t_why = nullptr;
  }
  why_scope(why_scope const &) = delete;
  why_scope &operator=(why_scope const &) = delete;
};

// "" for a null view's data, so an empty string is a syntax error with a message.
char const *data_of(std::string_view s) { return s.data() ? s.data() : ""; }

}  // namespace

int semver_parse(nsv_semver_t &out, std::string_view s, std::string *why) {
  why_scope const scope{ why };
  return nsv_parse(&out, data_of(s), s.size());
}

int semver_range_match(std::string_view range, nsv_semver_t const &v, std::string *why) {
  why_scope const scope{ why };
  return nsv_range_match(data_of(range), range.size(), &v, 0);
}

}  // namespace envy
