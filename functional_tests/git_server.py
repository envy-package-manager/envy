"""A git:// server for functional tests, safe on Windows where `git daemon` is not."""

from __future__ import annotations

import shutil
import socket
import subprocess
import threading
from pathlib import Path

_GIT = shutil.which("git")


class GitServer:
    """A git:// server: one `git upload-pack` per connection, relayed over pipes.

    Serves `git://127.0.0.1:{port}/<path>` from `root / <path>`, a bare repo or a .git.

    Not `git daemon`: on Windows it hands the socket itself to upload-pack, which exits
    holding it, with no shutdown or closesocket. Windows aborts a socket that dies with its
    process: a reset, which discards whatever the client has not read yet. libgit2 opens
    its pack file before it reads the pack, so a small response arrives whole, sits
    unread, and is lost. Relaying lets this server shut the socket down for writing and
    drain it, so every client reads to the end.
    """

    def __init__(self, root: Path, env: dict[str, str] | None = None):
        self.root, self.env = root, env
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.port = self.listener.getsockname()[1]
        threading.Thread(target=self._accept, daemon=True).start()

    def close(self) -> None:
        self.listener.close()

    def _accept(self) -> None:
        while True:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def _serve(self, conn: socket.socket) -> None:
        with conn:
            head = self._read_exact(conn, 4)
            if len(head) < 4:
                return  # closed without a request
            request = self._read_exact(conn, int(head, 16) - 4)
            command, _, rest = request.partition(b" ")
            path = rest.split(b"\0", 1)[0].decode().lstrip("/")
            if command != b"git-upload-pack":
                return
            proc = subprocess.Popen([_GIT, "upload-pack", "--timeout=0", str(self.root / path)],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    env=self.env)

            def to_server():
                try:
                    while chunk := conn.recv(65536):
                        proc.stdin.write(chunk)
                        proc.stdin.flush()
                except OSError:
                    pass
                finally:
                    try:
                        proc.stdin.close()
                    except OSError:
                        pass

            threading.Thread(target=to_server, daemon=True).start()
            try:
                with proc.stdout:
                    while chunk := proc.stdout.read1(65536):
                        conn.sendall(chunk)
            except OSError:
                pass
            proc.wait()
            # A graceful close: say we are done, then wait for the client to finish.
            try:
                conn.shutdown(socket.SHUT_WR)
                conn.settimeout(10)
                while conn.recv(65536):
                    pass
            except OSError:
                pass

    @staticmethod
    def _read_exact(conn: socket.socket, n: int) -> bytes:
        data = b""
        while len(data) < n:
            chunk = conn.recv(n - len(data))
            if not chunk:
                break
            data += chunk
        return data
