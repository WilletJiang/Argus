#!/usr/bin/env python3
"""Offline integration checks; argv[1] is the compiled http_fixture executable."""
import collections
import json
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def event(value):
    return b"data: " + json.dumps(value, ensure_ascii=False).encode() + b"\r\n\r\n"


RESPONSE = {"id": "r-test", "status": "completed", "output": [
    {"type": "function_call", "id": "fc", "call_id": "call", "name": "local.echo",
     "arguments": '{"text":"🌍"}', "status": "completed"}]}
TERMINAL = event({"type": "response.completed", "response": RESPONSE})
DELTA = event({"type": "response.output_text.delta", "delta": "hello 🌍"})
COUNTS = collections.Counter()
REQUEST_ERRORS = []


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_POST(self):
        try:
            self.respond()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def respond(self):
        COUNTS[self.path] += 1
        body = self.rfile.read(int(self.headers["Content-Length"]))
        if self.headers.get("Authorization") != "Bearer local-test-token":
            REQUEST_ERRORS.append("invalid authorization header")
        if json.loads(body).get("stream") is not True:
            REQUEST_ERRORS.append("request did not enable streaming")
        path = self.path
        status = 200
        content_type = "text/event-stream; charset=utf-8"
        payload = DELTA + TERMINAL
        headers = {}
        if path in ("/429", "/500") and COUNTS[path] < 3:
            status = int(path[1:])
            content_type = "application/json"
            payload = b'{"error":{"message":"transient"}}'
        elif path in ("/401", "/403", "/400"):
            status = int(path[1:])
            content_type = "application/json"
            payload = b'{"error":{"message":"rejected"}}'
        elif path == "/redirect":
            status = 307
            headers["Location"] = "/redirect-target"
            payload = b""
        elif path == "/wrong-mime":
            content_type = "application/json"
        elif path == "/no-mime-sse":
            content_type = None
        elif path == "/no-mime-html":
            content_type = None
            payload = b"<html><body>upstream error</body></html>"
        elif path == "/no-mime-truncated":
            content_type = None
            payload = DELTA
        elif path == "/truncated":
            payload = DELTA
        elif path == "/duplicate":
            payload = TERMINAL + TERMINAL
        elif path == "/malformed":
            payload = b"data: {broken}\n\n" + TERMINAL
        elif path == "/utf8":
            payload = b'data: {"type":"x","delta":"\xf0\x80\x80\x80"}\n\n'
        elif path == "/nul-type":
            payload = event({"type": "response.completed\0ignored"})
        elif path == "/nul-delta":
            payload = event({"type": "response.output_text.delta", "delta": "before\0after"})
        elif path == "/incomplete":
            payload = event({"type": "response.incomplete", "response": {
                "incomplete_details": {"reason": "max_output_tokens"}}})
        elif path == "/large":
            payload = b":" + b"x" * 9000
        elif path == "/large-error":
            status = 500
            payload = b"x" * 10000
        elif path == "/backoff":
            status = 429
            headers["Retry-After"] = "30"
        elif path in ("/hang", "/timeout"):
            payload = b""
        elif path == "/after-delta":
            payload = DELTA
        elif path == "/before-output" and COUNTS[path] < 3:
            payload = event({"type": "response.created", "response": {"status": "in_progress"}})
        self.send_response(status)
        if content_type is not None:
            self.send_header("Content-Type", content_type)
        if path == "/duplicate-mime":
            self.send_header("Content-Type", content_type)
        incomplete = path in ("/hang", "/timeout", "/after-delta") or (
            path == "/before-output" and COUNTS[path] < 3)
        self.send_header("Content-Length", str(len(payload) + (100 if incomplete else 0)))
        for name, value in headers.items():
            self.send_header(name, value)
        self.end_headers()
        if path == "/fragmented":
            for byte in payload:
                self.wfile.write(bytes([byte]))
                self.wfile.flush()
        else:
            self.wfile.write(payload)
            self.wfile.flush()
        if path in ("/hang", "/timeout"):
            time.sleep(2)
        if incomplete:
            self.close_connection = True


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    endpoint = f"http://127.0.0.1:{server.server_port}"

    def run(path, code=0, requests=1, timeout=4000, cancel=0, repeat=False):
        before = COUNTS[path]
        completed = subprocess.run(
            [sys.argv[1], endpoint + path, str(timeout), str(cancel), "repeat" if repeat else "once"],
            text=True, capture_output=True, timeout=8, check=True)
        value = json.loads(completed.stdout)
        assert value["code"] == code, (path, value, completed.stderr)
        assert COUNTS[path] - before == requests, (path, COUNTS[path] - before)
        if code:
            assert value["response"] is None, (path, value)
        else:
            assert value["response"] == RESPONSE, (path, value)
        return value

    try:
        run("/ok")
        run("/no-mime-sse")
        run("/fragmented")
        run("/reuse", requests=2, repeat=True)
        run("/429", requests=3)
        run("/500", requests=3)
        run("/before-output", requests=3)
        run("/401", code=9)
        run("/403", code=9)
        run("/400", code=3)
        run("/redirect", code=3)
        assert COUNTS["/redirect-target"] == 0
        for path in ("/wrong-mime", "/duplicate-mime", "/truncated", "/duplicate", "/malformed", "/utf8"):
            run(path, code=3)
        run("/no-mime-html", code=3)
        run("/no-mime-truncated", code=3)
        for path in ("/nul-type", "/nul-delta"):
            assert run(path, code=3)["deltas"] == 0
        run("/incomplete", code=4)
        run("/large", code=4)
        run("/large-error", code=4)
        value = run("/after-delta", code=10)
        assert value["deltas"] == len("hello 🌍".encode())
        for path in ("/hang", "/backoff"):
            value = run(path, code=5, cancel=120)
            assert 100 <= value["elapsed"] < 600, (path, value)
        value = run("/timeout", code=4, timeout=150)
        assert 100 <= value["elapsed"] < 600, value
        assert not REQUEST_ERRORS, REQUEST_ERRORS
        print("http: local streaming, retries, auth, limits, reuse, cancellation passed")
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


if __name__ == "__main__":
    main()
