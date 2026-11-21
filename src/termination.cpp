#include "termination.h"

#ifdef _WIN32

#include "platform.h"

namespace {

BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
  switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
      // Restore cursor visibility and auto-wrap before exit
      ::WriteFile(::GetStdHandle(STD_ERROR_HANDLE),
                  "\x1b[?25h\x1b[?7h",
                  12,
                  nullptr,
                  nullptr);
      ::ExitProcess(130);
    default: return FALSE;
  }
}

}  // namespace

namespace envy {

void termination_handler_install() { ::SetConsoleCtrlHandler(console_ctrl_handler, TRUE); }

void termination_restore_inherited() {}  // console handlers are never inherited

}  // namespace envy

#else  // POSIX

#include <unistd.h>

#include <csignal>
#include <tuple>

namespace {

struct sigaction s_inherited_int{};
struct sigaction s_inherited_term{};
struct sigaction s_inherited_pipe{};

void signal_handler(int sig) {
  // Restore cursor visibility and auto-wrap before exit
  std::ignore = write(STDERR_FILENO, "\x1b[?25h\x1b[?7h", 12);
  _exit(128 + sig);
}

}  // namespace

namespace envy {

void termination_handler_install() {
  struct sigaction sa{};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);

  sigaction(SIGINT, &sa, &s_inherited_int);
  sigaction(SIGTERM, &sa, &s_inherited_term);

  // Ignore SIGPIPE: libgit2/libcurl/libssh2 can write to broken sockets
  // during network operations; the default SIGPIPE kills the process.
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, &s_inherited_pipe);
}

void termination_restore_inherited() {
  sigaction(SIGINT, &s_inherited_int, nullptr);
  sigaction(SIGTERM, &s_inherited_term, nullptr);
  sigaction(SIGPIPE, &s_inherited_pipe, nullptr);
}

}  // namespace envy

#endif
