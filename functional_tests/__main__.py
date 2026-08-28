from __future__ import annotations

import faulthandler
import os
import pathlib
import sys
import threading
import time
import traceback
import unittest
from concurrent.futures import ThreadPoolExecutor, as_completed

from . import test_config

# Watchdog: tests currently executing, name -> (monotonic start, timeout seconds).
# A test that exceeds its timeout terminates the whole run - a hung test must fail
# loudly, not stall forever (as_completed() blocks before any future timeout can fire).
# The default is deliberately tight (catches deadlocks in our code); inherently slow
# tests (network clones, downloads) opt into a larger budget via an
# `envy_watchdog_timeout` attribute on the test method or its class.
_running_tests: dict[str, tuple[float, float]] = {}
_running_tests_lock = threading.Lock()
_default_watchdog_timeout: float = 5.0


def _resolve_timeout(test: unittest.TestCase, default: float) -> float:
    method = getattr(test, test._testMethodName, None)
    override = getattr(method, "envy_watchdog_timeout", None)
    if override is None:
        override = getattr(test, "envy_watchdog_timeout", None)
    return float(override) if override is not None else default


def _watchdog_loop() -> None:
    while True:
        time.sleep(0.5)
        now = time.monotonic()
        with _running_tests_lock:
            for name, (start, timeout) in _running_tests.items():
                if now - start > timeout:
                    sys.stderr.write(
                        f"\nwatchdog: test '{name}' exceeded {timeout:.0f}s; "
                        "terminating\n"
                    )
                    sys.stderr.flush()
                    faulthandler.dump_traceback()
                    os._exit(1)

# Track thread exceptions to catch subprocess decode errors
_thread_exceptions: list[tuple[str, str]] = []
_original_excepthook = threading.excepthook


def _thread_excepthook(args):
    """Capture unhandled exceptions in threads."""
    exc_str = "".join(
        traceback.format_exception(args.exc_type, args.exc_value, args.exc_tb)
    )
    _thread_exceptions.append((args.thread.name if args.thread else "unknown", exc_str))
    _original_excepthook(args)


threading.excepthook = _thread_excepthook


def _disable_fetch_retry_by_default() -> None:
    """Fetches fail fast unless a test asks otherwise.

    Plenty of cases point envy at a dead port on purpose. Production retries a refused
    connection three times with seconds of backoff between, which would push every one
    of them past the watchdog for no coverage. test_fetch_retry.py, which is the file
    that actually tests the policy, sets ENVY_FETCH_ATTEMPTS back up for itself.
    """
    os.environ.setdefault("ENVY_FETCH_ATTEMPTS", "1")


def _setup_sanitizer_env() -> None:
    """Set up sanitizer environment variables and print configuration for debugging."""
    # Sanitizers not supported on Windows
    if sys.platform == "win32":
        return

    root = pathlib.Path(__file__).resolve().parent.parent
    tsan_supp = root / "tsan.supp"
    asan_supp = root / "asan.supp"

    print(f"Sanitizer suppression files:")
    print(f"  TSAN: {tsan_supp} (exists: {tsan_supp.exists()})")
    print(f"  ASAN: {asan_supp} (exists: {asan_supp.exists()})")

    # Actually set the environment variables so child processes inherit them
    if tsan_supp.exists() and "TSAN_OPTIONS" not in os.environ:
        os.environ["TSAN_OPTIONS"] = f"suppressions={tsan_supp}"
    if asan_supp.exists() and "ASAN_OPTIONS" not in os.environ:
        os.environ["ASAN_OPTIONS"] = f"suppressions={asan_supp}"

    print(f"Sanitizer environment:")
    print(f"  TSAN_OPTIONS: {os.environ.get('TSAN_OPTIONS', '(not set)')}")
    print(f"  ASAN_OPTIONS: {os.environ.get('ASAN_OPTIONS', '(not set)')}")
    print()


def _build_default_argv() -> list[str]:
    root = pathlib.Path(__file__).resolve().parent
    return [
        "functional_tests",
        "discover",
        "-s",
        str(root),
        "-t",
        str(root.parent),
    ]


def _flatten_suite(suite: unittest.TestSuite) -> list[unittest.TestCase]:
    """Recursively flatten a test suite into individual test cases."""
    tests = []
    for test in suite:
        if isinstance(test, unittest.TestSuite):
            tests.extend(_flatten_suite(test))
        else:
            tests.append(test)
    return tests


def _run_single_test(
    test: unittest.TestCase, verbose: bool = False
) -> unittest.TestResult:
    """Run a single test case and return its result."""
    test_name = f"{test.__class__.__module__}.{test.__class__.__name__}.{test._testMethodName}"
    if verbose:
        print(f"\nRunning: {test_name}", flush=True)

    suite = unittest.TestSuite([test])
    result = unittest.TestResult()
    timeout = _resolve_timeout(test, _default_watchdog_timeout)
    with _running_tests_lock:
        _running_tests[test_name] = (time.monotonic(), timeout)
    try:
        suite.run(result)
    finally:
        with _running_tests_lock:
            _running_tests.pop(test_name, None)

    return result


def _get_test_timeout() -> int:
    """Get per-test watchdog timeout in seconds from ENVY_TEST_TIMEOUT (default: 5)."""
    timeout_env = os.environ.get("ENVY_TEST_TIMEOUT")
    if timeout_env:
        try:
            t = int(timeout_env)
            if t >= 1:
                return t
            print(
                f"Warning: Invalid ENVY_TEST_TIMEOUT={timeout_env} (must be >= 1), using 5"
            )
        except ValueError:
            print(
                f"Warning: Invalid ENVY_TEST_TIMEOUT={timeout_env} (not a number), using 5"
            )
    return 5


def _run_parallel(
    loader: unittest.TestLoader, root: pathlib.Path, jobs: int, verbose: bool = False
) -> None:
    """Run tests in parallel using ThreadPoolExecutor."""
    global _default_watchdog_timeout
    start_time = time.time()
    test_timeout = _get_test_timeout()
    _default_watchdog_timeout = float(test_timeout)

    # Discover all tests
    suite = loader.discover(str(root), top_level_dir=str(root.parent))
    test_cases = _flatten_suite(suite)

    if len(test_cases) == 0:
        print("No tests found")
        return

    # Verify functional tester exists
    try:
        exe = test_config.get_envy_executable()
        if verbose:
            print(f"Using functional tester: {exe}")
    except RuntimeError as e:
        print(f"ERROR: {e}")
        sys.exit(1)

    threading.Thread(target=_watchdog_loop, daemon=True).start()

    timeout_note = f", watchdog={test_timeout}s" if test_timeout != 5 else ""
    print(
        f"Running {len(test_cases)} tests with {jobs} workers{timeout_note}..."
        + (" (verbose mode)" if verbose else "")
    )

    # Counters for results
    passed = 0
    failed = 0
    errors = 0
    skipped = 0
    failure_details = []

    # Run tests in parallel
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        future_to_test = {
            executor.submit(_run_single_test, test, verbose): test
            for test in test_cases
        }

        for i, future in enumerate(as_completed(future_to_test), 1):
            test = future_to_test[future]
            try:
                result = future.result()
            except Exception as e:
                sys.stdout.write("E")
                errors += 1
                failure_details.append(
                    ("ERROR", str(test), f"Test execution failed: {e}\n")
                )
                sys.stdout.flush()
                continue

            if result.errors:
                sys.stdout.write("E")
                errors += 1
                for test_case, traceback in result.errors:
                    failure_details.append(("ERROR", test_case, traceback))
            elif result.failures:
                sys.stdout.write("F")
                failed += 1
                for test_case, traceback in result.failures:
                    failure_details.append(("FAIL", test_case, traceback))
            elif result.skipped:
                sys.stdout.write("s")
                skipped += 1
            else:
                sys.stdout.write(".")
                passed += 1

            sys.stdout.flush()

            if i % 70 == 0:
                sys.stdout.write("\n")

    sys.stdout.write("\n")

    if failure_details:
        sys.stdout.write("=" * 70 + "\n")
        for failure_type, test_case, traceback in failure_details:
            sys.stdout.write(f"{failure_type}: {test_case}\n")
            sys.stdout.write("-" * 70 + "\n")
            sys.stdout.write(traceback)
            sys.stdout.write("\n")

    elapsed = time.time() - start_time
    sys.stdout.write("-" * 70 + "\n")
    sys.stdout.write(f"Ran {len(test_cases)} tests in {elapsed:.3f}s\n")
    sys.stdout.write("\n")

    # Check for thread exceptions (e.g., subprocess decode errors)
    if _thread_exceptions:
        sys.stdout.write("=" * 70 + "\n")
        sys.stdout.write(f"THREAD EXCEPTIONS ({len(_thread_exceptions)}):\n")
        sys.stdout.write("-" * 70 + "\n")
        for thread_name, exc_str in _thread_exceptions:
            sys.stdout.write(f"Thread: {thread_name}\n{exc_str}\n")
        errors += len(_thread_exceptions)

    if failed + errors > 0:
        parts = []
        if failed:
            parts.append(f"failures={failed}")
        if errors:
            parts.append(f"errors={errors}")
        if skipped:
            parts.append(f"skipped={skipped}")
        sys.stdout.write(f"FAILED ({', '.join(parts)})\n")
        sys.exit(1)
    else:
        if skipped:
            sys.stdout.write(f"OK (skipped={skipped})\n")
        else:
            sys.stdout.write("OK\n")


def main() -> None:
    _setup_sanitizer_env()
    _disable_fetch_retry_by_default()

    # Determine number of jobs for parallel execution
    jobs_env = os.environ.get("ENVY_TEST_JOBS")
    verbose = os.environ.get("ENVY_TEST_VERBOSE", "").lower() in ("1", "true", "yes")

    if jobs_env:
        if jobs_env.lower() == "sequential" or jobs_env == "0":
            jobs = None
        else:
            try:
                jobs = int(jobs_env)
                if jobs < 1:
                    print(
                        f"Warning: Invalid ENVY_TEST_JOBS={jobs_env} (must be >= 1), using auto"
                    )
                    jobs = os.cpu_count() or 4
            except ValueError:
                print(
                    f"Warning: Invalid ENVY_TEST_JOBS={jobs_env} (not a number), using auto"
                )
                jobs = os.cpu_count() or 4
    else:
        jobs = os.cpu_count() or 4

    if jobs and jobs > 1:
        root = pathlib.Path(__file__).resolve().parent
        loader = unittest.TestLoader()
        _run_parallel(loader, root, jobs, verbose)
        return

    argv = sys.argv if len(sys.argv) > 1 else _build_default_argv()
    if verbose:
        argv.append("-v")
    # exit=False so the thread-exception check below is reachable: a stub server that dies
    # in a handler thread must fail the sequential run too, not just the parallel one.
    program = unittest.main(module=None, argv=argv, exit=False)
    if _thread_exceptions:
        sys.stderr.write(f"\nTHREAD EXCEPTIONS ({len(_thread_exceptions)}):\n")
        for thread_name, exc_str in _thread_exceptions:
            sys.stderr.write(f"Thread: {thread_name}\n{exc_str}\n")
    sys.exit(0 if program.result.wasSuccessful() and not _thread_exceptions else 1)


if __name__ == "__main__":
    main()
