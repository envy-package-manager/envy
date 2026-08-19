#if defined(_WIN32)
#error "shell_posix.cpp should not be compiled on Windows builds"
#else

#include "shell.h"

#include "tui.h"
#include "util.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace envy {
namespace {

constexpr int kChildErrorExit{ 127 };
constexpr int kSignalExitBase{ 128 };
constexpr int kPollIntervalMs{ 100 };  // bounds how long child exit can go unnoticed
constexpr size_t kChunkSize{ 4096 };

class fd_cleanup {
 public:
  explicit fd_cleanup(int fd) : fd_{ fd } {}
  ~fd_cleanup() {
    if (fd_ == -1) { return; }
    close_with_retry();
  }

  fd_cleanup(fd_cleanup const &) = delete;
  fd_cleanup &operator=(fd_cleanup const &) = delete;
  fd_cleanup(fd_cleanup &&other) noexcept : fd_{ other.fd_ } { other.fd_ = -1; }

  fd_cleanup &operator=(fd_cleanup &&other) noexcept {
    if (this == &other) { return *this; }
    close_and_take(other);
    return *this;
  }

  int get() const { return fd_; }

  void release() {
    if (fd_ == -1) { return; }
    close_with_retry();
    fd_ = -1;
  }

 private:
  void close_with_retry() {
    for (int attempts{ 0 }; attempts < 3 && ::close(fd_) == -1; ++attempts) {
      if (errno != EINTR) { break; }
    }
  }

  void close_and_take(fd_cleanup &other) {
    if (fd_ != -1) { close_with_retry(); }
    fd_ = other.fd_;
    other.fd_ = -1;
  }

  int fd_{ -1 };
};

std::vector<std::string> get_shell_argv(shell_choice choice) {
  switch (choice) {
    case shell_choice::bash:
      if (char const *bash_env{ ::getenv("BASH") }) { return { bash_env, "-e" }; }
      return { "/usr/bin/env", "bash", "-e" };

    case shell_choice::sh: return { "/bin/sh", "-e" };

    default: throw std::invalid_argument("shell_run: unsupported shell choice on POSIX");
  }
}

void write_all(int fd, char const *data, size_t size) {
  ssize_t remaining{ static_cast<ssize_t>(size) };
  while (remaining > 0) {
    ssize_t const written{ ::write(fd, data, remaining) };
    if (written == -1) {
      if (errno == EINTR) { continue; }
      throw std::system_error(errno, std::generic_category(), "write failed");
    }
    remaining -= written;
    data += written;
  }
}

std::string create_temp_script(std::string_view script) {
  auto tmp_dir{ std::filesystem::temp_directory_path() };
  std::string pattern{ (tmp_dir / "envy-shell-XXXXXX").string() };

  std::vector<char> path_buffer{ pattern.begin(), pattern.end() };
  path_buffer.push_back('\0');

  int const fd{ ::mkstemp(path_buffer.data()) };
  if (fd == -1) {
    throw std::system_error(errno, std::generic_category(), "mkstemp failed");
  }
  fd_cleanup fd_guard{ fd };

  std::string content{ script };
  if (!content.empty() && content.back() != '\n') { content.push_back('\n'); }

  write_all(fd_guard.get(), content.data(), content.size());

  if (::fchmod(fd_guard.get(), S_IRUSR | S_IWUSR | S_IXUSR) == -1) {
    throw std::system_error(errno, std::generic_category(), "fchmod failed");
  }

  if (::fsync(fd_guard.get()) == -1) {
    throw std::system_error(errno, std::generic_category(), "fsync failed");
  }

  return std::string{ path_buffer.data() };
}

struct pipe_state {
  fd_cleanup read_fd;
  shell_stream stream;
  std::string pending;
  bool closed;
};

void dispatch_line(pipe_state const &pipe, std::string_view line,
                   shell_run_cfg const &cfg) {
  if (pipe.stream == shell_stream::std_out) {
    if (cfg.on_stdout_line) { cfg.on_stdout_line(line); }
  } else {
    if (cfg.on_stderr_line) { cfg.on_stderr_line(line); }
  }
  if (cfg.on_output_line) { cfg.on_output_line(line); }
}

void emit_complete_lines(pipe_state &pipe, shell_run_cfg const &cfg) {
  size_t newline{ 0 };
  while ((newline = pipe.pending.find('\n')) != std::string::npos) {
    dispatch_line(pipe, std::string_view{ pipe.pending }.substr(0, newline), cfg);
    pipe.pending.erase(0, newline + 1);
  }
}

void close_pipe(pipe_state &pipe, shell_run_cfg const &cfg) {
  if (!pipe.pending.empty()) {  // trailing partial line
    dispatch_line(pipe, pipe.pending, cfg);
    pipe.pending.clear();
  }
  pipe.closed = true;
}

enum class read_status { progress, would_block, eof };

read_status read_pipe(pipe_state &pipe, shell_run_cfg const &cfg, std::string &chunk) {
  ssize_t const read_bytes{ ::read(pipe.read_fd.get(), chunk.data(), chunk.size()) };
  if (read_bytes == 0) { return read_status::eof; }
  if (read_bytes == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) { return read_status::would_block; }
    if (errno == EINTR) { return read_status::progress; }
    throw std::system_error(errno, std::generic_category(), "read failed");
  }
  pipe.pending.append(chunk.data(), static_cast<size_t>(read_bytes));
  emit_complete_lines(pipe, cfg);
  return read_status::progress;
}

class child_process {
 public:
  explicit child_process(pid_t pid) : pid_{ pid } {}
  ~child_process() {
    if (pid_ == -1) { return; }
    ::kill(pid_, SIGKILL);
    reap(true);
  }

  child_process(child_process const &) = delete;
  child_process &operator=(child_process const &) = delete;

  bool try_reap() { return pid_ == -1 || reap(false); }  // non-blocking exit check

  shell_result wait() {
    if (pid_ != -1 && !reap(true)) {
      throw std::system_error(errno, std::generic_category(), "waitpid failed");
    }
    return result_;
  }

 private:
  bool reap(bool block) {
    int status{ 0 };
    pid_t result{ 0 };
    while ((result = ::waitpid(pid_, &status, block ? 0 : WNOHANG)) == -1 &&
           errno == EINTR) {}
    if (result != pid_) { return false; }
    pid_ = -1;
    if (WIFEXITED(status)) {
      result_ = { .exit_code = WEXITSTATUS(status), .signal = std::nullopt };
    } else if (WIFSIGNALED(status)) {
      int const sig{ WTERMSIG(status) };
      result_ = { .exit_code = kSignalExitBase + sig, .signal = sig };
    } else {
      result_ = { .exit_code = status, .signal = std::nullopt };
    }
    return true;
  }

  pid_t pid_;
  shell_result result_{};
};

// The child is gone; flush what is already buffered instead of waiting for EOF, which a
// surviving descendant holding an inherited write end would delay for its own lifetime.
void drain_after_exit(std::array<pipe_state, 2> &pipes, shell_run_cfg const &cfg) {
  std::string chunk(kChunkSize, '\0');
  bool inherited{ false };

  for (auto &pipe : pipes) {
    if (pipe.closed) { continue; }

    int const flags{ ::fcntl(pipe.read_fd.get(), F_GETFL, 0) };
    if (flags == -1 || ::fcntl(pipe.read_fd.get(), F_SETFL, flags | O_NONBLOCK) == -1) {
      throw std::system_error(errno, std::generic_category(), "fcntl failed");
    }

    read_status status{ read_status::progress };
    while (status == read_status::progress) { status = read_pipe(pipe, cfg, chunk); }
    inherited = inherited || status == read_status::would_block;
    close_pipe(pipe, cfg);
  }

  if (inherited) {
    tui::debug("shell: child exited with output pipes still held by a descendant; "
               "output written after this point is dropped");
  }
}

void stream_pipes(std::array<pipe_state, 2> &pipes, child_process &child,
                  shell_run_cfg const &cfg) {
  std::array<pollfd, 2> poll_fds{};
  std::string chunk(kChunkSize, '\0');
  size_t closed_count{ 0 };

  for (size_t i{ 0 }; i < pipes.size(); ++i) {
    poll_fds[i].fd = pipes[i].read_fd.get();
    poll_fds[i].events = POLLIN;
    poll_fds[i].revents = 0;
  }

  while (closed_count < pipes.size()) {
    if (::poll(poll_fds.data(), poll_fds.size(), kPollIntervalMs) == -1) {
      if (errno == EINTR) { continue; }
      throw std::system_error(errno, std::generic_category(), "poll failed");
    }

    for (size_t i{ 0 }; i < pipes.size(); ++i) {
      short const revents{ poll_fds[i].revents };
      if (pipes[i].closed || revents == 0) { continue; }
      if (revents & (POLLERR | POLLNVAL)) {
        throw std::runtime_error("poll failed on child pipe");
      }
      if (read_pipe(pipes[i], cfg, chunk) != read_status::eof) { continue; }

      close_pipe(pipes[i], cfg);
      ++closed_count;
      poll_fds[i].fd = -1;
      poll_fds[i].events = 0;
      poll_fds[i].revents = 0;
    }

    // Pipe EOF means every writer let go, not that the child exited - poll for exit too.
    if (child.try_reap()) {
      drain_after_exit(pipes, cfg);
      return;
    }
  }
}

[[noreturn]] void exec_child_process(fd_cleanup &stdout_read,
                                     fd_cleanup &stdout_write,
                                     fd_cleanup &stderr_read,
                                     fd_cleanup &stderr_write,
                                     std::optional<std::filesystem::path> const &cwd,
                                     std::vector<std::string> const &argv_strings,
                                     std::vector<char *> const &envp) {
  stdout_read.release();
  stderr_read.release();

  int const null_fd{ ::open("/dev/null", O_RDONLY) };
  std::array<std::pair<int, int>, 3> const fd_mappings{
    std::pair{ null_fd, STDIN_FILENO },
    std::pair{ stdout_write.get(), STDOUT_FILENO },
    std::pair{ stderr_write.get(), STDERR_FILENO },
  };

  if (null_fd == -1) {
    std::perror("open /dev/null");
    _exit(kChildErrorExit);
  }

  for (auto const &[src, dst] : fd_mappings) {
    if (::dup2(src, dst) == -1) {
      std::perror("dup2");
      _exit(kChildErrorExit);
    }
  }

  if (null_fd != STDIN_FILENO) { ::close(null_fd); }
  stdout_write.release();
  stderr_write.release();

  if (cwd) {
    if (::chdir(cwd->c_str()) == -1) {
      std::perror("chdir");
      _exit(kChildErrorExit);
    }
  }

  if (argv_strings.empty()) {
    std::fprintf(stderr, "exec_child_process: argv must be non-empty\n");
    _exit(kChildErrorExit);
  }

  std::vector<char *> argv;
  argv.reserve(argv_strings.size() + 1);
  for (auto const &arg : argv_strings) { argv.push_back(const_cast<char *>(arg.c_str())); }
  argv.push_back(nullptr);

  ::execve(argv_strings[0].c_str(), argv.data(), const_cast<char **>(envp.data()));
  std::perror("execve");
  _exit(kChildErrorExit);
}

}  // namespace

void shell_init() {
  // No-op on POSIX - no job object initialization needed
}

shell_env_t shell_getenv() {
  shell_env_t env;
  if (!environ) { return env; }

  for (char **entry{ environ }; *entry != nullptr; ++entry) {
    std::string_view kv{ *entry };
    size_t const sep{ kv.find('=') };
    if (sep == std::string_view::npos) { continue; }
    std::string key{ kv.substr(0, sep) };
    std::string value{ kv.substr(sep + 1) };
    env[std::move(key)] = std::move(value);
  }

  return env;
}

shell_result shell_run(std::string_view script, shell_run_cfg const &cfg) {
  // Determine if we need a script file or pass inline
  bool const is_inline{ std::holds_alternative<custom_shell_inline>(cfg.shell) };

  std::string script_path{};
  std::unique_ptr<scoped_path_cleanup> cleanup;

  if (!is_inline) {
    script_path = create_temp_script(script);
    cleanup = std::make_unique<scoped_path_cleanup>(std::filesystem::path{ script_path });
  }

  // Build argv based on shell type
  std::vector<std::string> argv_strings{ std::visit(
      match{
          [&script_path](shell_choice const &shell_cfg) -> std::vector<std::string> {
            if (shell_cfg != shell_choice::bash && shell_cfg != shell_choice::sh) {
              throw std::invalid_argument(
                  "shell_run: POSIX only supports bash or sh built-in shells");
            }
            auto args{ get_shell_argv(shell_cfg) };
            args.push_back(script_path);
            return args;
          },
          [&script_path](custom_shell_file const &shell_cfg) -> std::vector<std::string> {
            auto result{ shell_cfg.argv };
            result.push_back(script_path);
            return result;
          },
          [&script](custom_shell_inline const &shell_cfg) -> std::vector<std::string> {
            auto result{ shell_cfg.argv };
            result.push_back(std::string{ script });
            return result;
          },
      },
      cfg.shell) };

  auto const [env_strings, envp]{ [&cfg] {
    std::vector<std::string> strings;
    std::vector<char *> pointers;
    strings.reserve(cfg.env.size());
    pointers.reserve(cfg.env.size() + 1);
    for (auto const &[key, value] : cfg.env) { strings.push_back(key + "=" + value); }
    for (auto &entry : strings) { pointers.push_back(entry.data()); }
    pointers.push_back(nullptr);
    return std::pair{ std::move(strings), std::move(pointers) };
  }() };

  int stdout_pipefd[2];
  if (::pipe(stdout_pipefd) == -1) {
    throw std::system_error(errno, std::generic_category(), "pipe failed");
  }

  int stderr_pipefd[2];
  if (::pipe(stderr_pipefd) == -1) {
    throw std::system_error(errno, std::generic_category(), "pipe failed");
  }

  fd_cleanup stdout_read_end{ stdout_pipefd[0] };
  fd_cleanup stdout_write_end{ stdout_pipefd[1] };
  fd_cleanup stderr_read_end{ stderr_pipefd[0] };
  fd_cleanup stderr_write_end{ stderr_pipefd[1] };

  pid_t const child{ ::fork() };
  if (child == -1) {
    throw std::system_error(errno, std::generic_category(), "fork failed");
  }

  if (child == 0) {  // child process exits in exec_child_process
    exec_child_process(stdout_read_end,
                       stdout_write_end,
                       stderr_read_end,
                       stderr_write_end,
                       cfg.cwd,
                       argv_strings,
                       envp);
  }

  stdout_write_end.release();  // Parent: close write ends and stream output
  stderr_write_end.release();

  child_process child_proc{ child };  // kills and reaps if streaming throws
  std::array<pipe_state, 2> pipes{
    pipe_state{ std::move(stdout_read_end), shell_stream::std_out, {}, false },
    pipe_state{ std::move(stderr_read_end), shell_stream::std_err, {}, false },
  };

  stream_pipes(pipes, child_proc, cfg);
  return child_proc.wait();
}

}  // namespace envy

#endif  // POSIX implementation
