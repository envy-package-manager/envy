"""A local S3-compatible endpoint, reached via AWS_ENDPOINT_URL_S3.

Just enough S3 for envy's transfers: single-part PutObject, ranged GetObject, HeadObject.
Only `test-bucket` exists. Path-style addressing is assumed: clients pick it when the
endpoint host is an IP literal.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from xml.sax.saxutils import escape

from . import test_config

BUCKET = "test-bucket"


_PROXY_VARS = {"HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY"}


def hermetic_env(extra: dict[str, str]) -> dict[str, str]:
    """os.environ without the runner's AWS settings or proxies, then `extra` on top.

    A developer's AWS_SESSION_TOKEN, AWS_IGNORE_CONFIGURED_ENDPOINT_URLS or HTTPS_PROXY
    would otherwise sign, reroute or proxy requests meant for the stub.
    """
    env = {
        k: v
        for k, v in os.environ.items()
        if not k.upper().startswith("AWS_") and k.upper() not in _PROXY_VARS
    }
    env.update(extra)
    return env


def crc64nvme(data: bytes) -> int:
    """S3's CRC64NVME: reflected 0x9A6C9329AC4BC9B5, all-ones init and final xor."""
    crc = 0xFFFFFFFFFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x9A6C9329AC4BC9B5 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFFFFFFFFFF


def s3_env(port: int) -> dict[str, str]:
    """Environment pointing every AWS client at the stub, with static test credentials."""
    return {
        "AWS_ENDPOINT_URL": f"http://127.0.0.1:{port}",
        "AWS_ENDPOINT_URL_S3": f"http://127.0.0.1:{port}",
        "AWS_REGION": "us-east-1",
        "AWS_DEFAULT_REGION": "us-east-1",
        "AWS_ACCESS_KEY_ID": "test",
        "AWS_SECRET_ACCESS_KEY": "test",
        "AWS_EC2_METADATA_DISABLED": "1",
        "AWS_MAX_ATTEMPTS": "2",
    }


class S3Stub:
    """Single-part only: Create/UploadPart/CompleteMultipartUpload answer 501, so a client
    that reaches for multipart fails loudly rather than silently skipping coverage.
    """

    def __init__(self) -> None:
        self.objects: dict[str, bytes] = {}
        # (status, <Code>, <Message>) to answer every PutObject/GetObject with, or None.
        self.put_error: tuple[int, str, str] | None = None
        self.get_error: tuple[int, str, str] | None = None
        self.get_failures = 0  # GETs answered get_failure_status before serving any
        self.get_failure_status = 500
        self.head_override: tuple[int, dict[str, str]] | None = None  # status, headers
        self.require_auth = False  # unsigned requests draw 403 AccessDenied
        # Stored CRC64NVME sent on HEAD, as S3 does for single-part uploads: "good",
        # "bad" (a checksum that doesn't match the bytes), or None.
        self.checksum: str | None = None
        # SSO-OIDC CreateToken (POST /token), reached via AWS_ENDPOINT_URL_SSO_OIDC: the
        # (status, JSON) answer, and every request body received.
        self.oidc_reply: tuple[int, dict[str, object]] = (
            200,
            {
                "accessToken": "refreshed-access-token",
                "expiresIn": 3600,
                "refreshToken": "rotated-refresh-token",
                "tokenType": "Bearer",
            },
        )
        self.oidc_requests: list[dict[str, object]] = []
        self.requests: list[dict[str, object]] = []  # method, key, signed, range, headers
        self._lock = threading.Lock()
        self._server: ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None
        self.port = 0

    def __enter__(self) -> "S3Stub":
        self.start()
        return self

    def __exit__(self, *exc: object) -> None:
        self.stop()

    def env(self) -> dict[str, str]:
        return hermetic_env(s3_env(self.port))

    def start(self) -> int:
        parent = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, fmt: str, *args: object) -> None:  # noqa: A003
                return

            def _key(self) -> str:
                return self.path.lstrip("/").split("?", 1)[0]

            def _record(self) -> tuple[str, str, bool]:
                bucket, _, obj = self._key().partition("/")
                signed = "Authorization" in self.headers
                with parent._lock:
                    parent.requests.append(
                        {
                            "method": self.command,
                            "key": obj,
                            "signed": signed,
                            "range": self.headers.get("Range"),
                            "headers": {k.lower(): v for k, v in self.headers.items()},
                        }
                    )
                return bucket, obj, signed

            def _read_body(self) -> bytes:
                """Read the request body, unwrapping both framings the SDK applies.

                A PutObject with a trailing checksum arrives as `Content-Encoding:
                aws-chunked`, with an `x-amz-trailer` checksum after the terminating zero
                chunk, possibly inside HTTP `Transfer-Encoding: chunked` too. Reading the
                body fully also keeps HTTP/1.1 keep-alive framing intact for the next
                request.
                """
                if "chunked" in (self.headers.get("Transfer-Encoding") or "").lower():
                    body = self._read_http_chunked()
                else:
                    length = int(self.headers.get("Content-Length") or 0)
                    body = self.rfile.read(length) if length else b""

                if "aws-chunked" in (self.headers.get("Content-Encoding") or "").lower():
                    body = self._decode_aws_chunked(body)
                return body

            def _read_http_chunked(self) -> bytes:
                chunks: list[bytes] = []
                while True:
                    line = self.rfile.readline().strip()
                    if not line:
                        continue
                    size = int(line.split(b";", 1)[0], 16)
                    if size == 0:
                        break
                    chunks.append(self.rfile.read(size))
                    self.rfile.read(2)  # chunk-terminating CRLF
                while True:  # trailer headers, terminated by a blank line
                    line = self.rfile.readline()
                    if not line or line in (b"\r\n", b"\n"):
                        break
                return b"".join(chunks)

            @staticmethod
            def _decode_aws_chunked(buf: bytes) -> bytes:
                out = bytearray()
                pos = 0
                while True:
                    eol = buf.find(b"\r\n", pos)
                    if eol < 0:
                        break
                    size = int(buf[pos:eol].split(b";", 1)[0] or b"0", 16)
                    pos = eol + 2
                    if size == 0:
                        break  # trailers follow; the payload is complete
                    out += buf[pos : pos + size]
                    pos += size + 2  # data plus its CRLF
                return bytes(out)

            def _refuse_unsigned(self, signed: bool) -> bool:
                if parent.require_auth and not signed:
                    parent._send_error(self, 403, "AccessDenied", "Access Denied")
                    return True
                return False

            def do_PUT(self) -> None:
                body = self._read_body()  # Drained even on failure, to keep framing.
                bucket, obj, signed = self._record()
                if self._refuse_unsigned(signed):
                    return
                if parent.put_error is not None:
                    parent._send_error(self, *parent.put_error)
                    return
                # Fail loudly if the stub mis-framed the body rather than silently storing
                # an empty object, which would make byte-for-byte assertions meaningless.
                declared = self.headers.get("x-amz-decoded-content-length")
                if declared is not None and len(body) != int(declared):
                    parent._send_error(self, 400, "IncompleteBody")
                    return
                if re.search(r"[?&](uploads|uploadId|partNumber)\b", self.path):
                    parent._send_error(self, 501, "NotImplemented")  # multipart
                    return
                if not obj:
                    parent._send_error(self, 400, "InvalidRequest")
                    return
                if bucket != BUCKET:
                    parent._send_error(self, 404, "NoSuchBucket")
                    return
                parent.objects[obj] = body
                self.send_response(200)
                self.send_header("ETag", f'"{hashlib.md5(body).hexdigest()}"')
                self.send_header("Content-Length", "0")
                self.end_headers()

            def do_POST(self) -> None:
                body = self._read_body()
                if self.path == "/token":
                    with parent._lock:
                        parent.oidc_requests.append(json.loads(body or b"{}"))
                    status, reply = parent.oidc_reply
                    payload = json.dumps(reply).encode()
                    self.send_response(status)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                    return
                self._record()
                parent._send_error(self, 501, "NotImplemented")

            def do_GET(self) -> None:
                bucket, obj, signed = self._record()
                if self._refuse_unsigned(signed):
                    return
                with parent._lock:
                    failing = parent.get_failures > 0
                    parent.get_failures -= int(failing)
                if failing:
                    status = parent.get_failure_status
                    code = "TooManyRequests" if status == 429 else "InternalError"
                    parent._send_error(self, status, code, "Try again")
                    return
                if parent.get_error is not None:
                    parent._send_error(self, *parent.get_error)
                    return
                if bucket != BUCKET:
                    parent._send_error(self, 404, "NoSuchBucket")
                    return
                if obj not in parent.objects:
                    parent._send_error(self, 404, "NoSuchKey", "The specified key does not exist.")
                    return
                body = parent.objects[obj]
                first, last = 0, len(body) - 1
                ranged = re.fullmatch(r"bytes=(\d+)-(\d*)", self.headers.get("Range") or "")
                if ranged:
                    first = int(ranged.group(1))
                    last = min(int(ranged.group(2) or last), last)
                    if first > last:
                        parent._send_error(self, 416, "InvalidRange")
                        return
                self.send_response(206 if ranged else 200)
                if ranged:
                    self.send_header("Content-Range", f"bytes {first}-{last}/{len(body)}")
                self.send_header("Content-Length", str(last - first + 1))
                self.send_header("ETag", f'"{hashlib.md5(body).hexdigest()}"')
                self.send_header("Accept-Ranges", "bytes")
                self.end_headers()
                self.wfile.write(body[first : last + 1])

            def do_HEAD(self) -> None:
                bucket, obj, signed = self._record()
                if parent.head_override is not None:
                    status, headers = parent.head_override
                    self.send_response(status)
                    for name, value in headers.items():
                        self.send_header(name, value)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                if parent.require_auth and not signed:
                    status = 403
                elif bucket != BUCKET or obj not in parent.objects:
                    status = 404
                else:
                    status = 200
                self.send_response(status)
                body = parent.objects.get(obj, b"") if status == 200 else b""
                self.send_header("Content-Length", str(len(body)))
                if status == 200:
                    self.send_header("ETag", f'"{hashlib.md5(body).hexdigest()}"')
                    self.send_header("Accept-Ranges", "bytes")
                    if parent.checksum is not None:
                        crc = crc64nvme(body) ^ (parent.checksum == "bad")
                        digest = base64.b64encode(crc.to_bytes(8, "big")).decode()
                        self.send_header("x-amz-checksum-crc64nvme", digest)
                        self.send_header("x-amz-checksum-type", "FULL_OBJECT")
                self.end_headers()

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self._server.server_address[1]
        self._thread = test_config.serve_in_thread(self._server)
        return self.port

    @staticmethod
    def _send_error(
        handler: BaseHTTPRequestHandler,
        code: int,
        aws_code: str,
        message: str | None = None,
    ) -> None:
        # Escaped: a caller-supplied message carrying & or < would otherwise emit invalid
        # XML, and the client would fail to parse it for a reason the test never intended.
        body = (
            f'<?xml version="1.0" encoding="UTF-8"?><Error><Code>{escape(aws_code)}</Code>'
            f"<Message>{escape(message or aws_code)}</Message></Error>"
        ).encode()
        handler.send_response(code)
        handler.send_header("Content-Type", "application/xml")
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)

    def stop(self) -> None:
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()
        if self._thread is not None:
            self._thread.join(timeout=5)
