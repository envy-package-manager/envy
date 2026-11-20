#pragma once

namespace envy {

// Install platform-specific termination handler (SIGINT/SIGTERM on POSIX,
// SetConsoleCtrlHandler on Windows). Handler calls _exit(130) immediately.
void termination_handler_install();

// Put back the SIGINT/SIGTERM/SIGPIPE dispositions envy inherited, ahead of an exec: an
// ignored SIGPIPE survives exec, so a tool behind `| head` would never die. POSIX only.
void termination_restore_inherited();

}  // namespace envy
