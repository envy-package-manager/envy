from __future__ import annotations

import pathlib
import socketserver
import sys
import unittest


def _handle_error(self, request, client_address) -> None:
    """Package-wide policy for exceptions raised inside a stub server's request handler.

    socketserver's default prints a traceback to stderr and swallows the exception:
    the worst of both worlds, since a run drowns in tracebacks and still reports OK.
    Only a client-side disconnect is benign here -- SChannel (under aws-c-io) and
    WinINet close pooled keep-alive sockets abortively, so a keep-alive stub sees a
    reset while blocked reading the next request line, after the exchange it was
    serving already completed. Every other handler exception is re-raised on the
    serving thread, where __main__'s threading.excepthook records it and fails the run.
    """
    if isinstance(
        exc := sys.exception(),
        (ConnectionAbortedError, ConnectionResetError, BrokenPipeError),
    ):
        return
    raise exc


socketserver.BaseServer.handle_error = _handle_error


def load_tests(
    loader: unittest.TestLoader, tests: unittest.TestSuite, pattern: str | None
) -> unittest.TestSuite:
    start_dir = pathlib.Path(__file__).resolve().parent
    discovered = loader.discover(
        start_dir=str(start_dir),
        pattern=pattern or "test_*.py",
        top_level_dir=str(start_dir.parent),
    )
    tests.addTests(discovered)
    return tests
