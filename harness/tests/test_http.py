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
EMPTY_RESPONSE = {"id": "r-test", "status": "completed", "output": []}
EMPTY_TERMINAL = event({"type": "response.completed", "response": EMPTY_RESPONSE})
EMPTY_ITEMS = [
    {"id": "empty-msg", "type": "message", "content": [{"type": "output_text", "text": ""}]},
    {"id": "empty-fn", "type": "function_call", "arguments": ""},
    {"id": "empty-custom", "type": "custom_tool_call", "input": ""},
    {"id": "reason", "type": "reasoning", "summary": [{"type": "summary_text", "text": "reason"}]},
]
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
        elif path == "/after-terminal":
            payload = TERMINAL
        elif path == "/empty-content":
            payload = (event({"type": "response.output_text.delta", "delta": ""}) +
                       event({"type": "response.function_call_arguments.delta", "delta": ""}) +
                       event({"type": "response.custom_tool_call_input.delta", "delta": ""}) +
                       event({"type": "response.reasoning_text.delta", "delta": "reason"}) +
                       EMPTY_TERMINAL)
        elif path == "/empty-items":
            payload = b"".join(event({"type": "response.output_item.done", "output_index": i,
                                      "item": item}) for i, item in enumerate(EMPTY_ITEMS)) + EMPTY_TERMINAL
        elif path == "/failed-before-content":
            payload = event({"type": "response.failed", "response": {
                "error": {"code": "rate_limit_exceeded"}}})
        elif path == "/phases":
            chunks = [
                event({"type": "response.created", "response": {"status": "in_progress"}}) +
                event({"type": "response.output_text.delta", "delta": ""}),
                event({"type": "response.function_call_arguments.delta", "delta": "{}"}),
                event({"type": "response.output_item.done", "output_index": 0, "item": RESPONSE["output"][0]}),
                TERMINAL,
            ]
            payload = b"".join(chunks)
        elif path == "/before-output" and COUNTS[path] < 3:
            payload = event({"type": "response.created", "response": {"status": "in_progress"}})
        self.send_response(status)
        if content_type is not None:
            self.send_header("Content-Type", content_type)
        if path == "/duplicate-mime":
            self.send_header("Content-Type", content_type)
        incomplete = path in ("/hang", "/timeout", "/after-delta", "/after-terminal") or (
            path == "/before-output" and COUNTS[path] < 3)
        self.send_header("Content-Length", str(len(payload) + (100 if incomplete else 0)))
        for name, value in headers.items():
            self.send_header(name, value)
        self.end_headers()
        if path == "/phases":
            for chunk in chunks:
                self.wfile.write(chunk)
                self.wfile.flush()
                time.sleep(0.04)
        elif path == "/fragmented":
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

    def run(path, code=0, requests=1, timeout=4000, cancel=0, repeat=False,
            expected=RESPONSE, observe=True, cancel_repeat=False, attempts=None):
        before = COUNTS[path]
        completed = subprocess.run(
            [sys.argv[1], endpoint + path, str(timeout), str(cancel),
             "cancel-repeat" if cancel_repeat else "repeat" if repeat else "once" if observe else "no-observer"],
            text=True, capture_output=True, timeout=8, check=True)
        value = json.loads(completed.stdout)
        assert value["code"] == code, (path, value, completed.stderr)
        assert COUNTS[path] - before == requests, (path, COUNTS[path] - before)
        if code:
            assert value["response"] is None, (path, value)
        else:
            assert value["response"] == expected, (path, value)
        assert value["observer_guarded"], (path, value)
        assert len(value["metrics"]) == ((attempts or requests) if observe else 0), (path, value)
        for metric in value["metrics"]:
            assert metric["request_bytes"] == len('{"model":"test","input":[],"stream":true}')
            assert metric["elapsed_present"] and metric["elapsed_us"] >= 0
            for name in ("first_byte", "first_content", "first_output_item", "terminal"):
                point = metric[name]
                assert not point["present"] or point["us"] <= metric["elapsed_us"], (path, name, metric)
            for name in ("dns", "connect", "tls", "pretransfer", "curl_first_byte", "curl_total"):
                assert metric[name]["present"] == metric["new_connections_present"], (path, name, metric)
        if observe and path != "/backoff":
            assert value["metrics"][-1]["result"] == code, (path, value)
        return value

    try:
        metric = run("/ok")["metrics"][0]
        assert metric["first_byte"]["present"] and metric["first_content"]["present"]
        assert metric["terminal"]["present"] and metric["terminal_completed"]
        assert metric["upload_complete"]["present"]
        assert metric["response_bytes"] == len(DELTA + TERMINAL)
        run("/ok", observe=False)
        run("/no-mime-sse")
        run("/fragmented")
        reused = run("/reuse", requests=2, repeat=True)["metrics"]
        assert [m["attempt"] for m in reused] == [1, 1]
        assert [m["new_connections"] for m in reused] == [1, 0]
        cancelled = run("/cancel-before-reuse", code=5, cancel_repeat=True, attempts=2)["metrics"][1]
        assert not cancelled["new_connections_present"]
        assert not cancelled["first_byte"]["present"] and not cancelled["terminal_completed"]
        retried = run("/429", requests=3)["metrics"]
        assert [m["attempt"] for m in retried] == [1, 2, 3]
        assert [m["retry_planned"] for m in retried] == [True, True, False]
        assert not retried[0]["first_content"]["present"]
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
        late = run("/after-terminal", code=10)["metrics"][0]
        assert late["terminal_completed"] and late["terminal"]["present"]
        assert late["result"] != 0 and not late["retry_planned"]
        empty = run("/empty-content", expected=EMPTY_RESPONSE)["metrics"][0]
        assert not empty["first_content"]["present"] and empty["terminal_completed"]
        empty_items = run("/empty-items", expected={**EMPTY_RESPONSE, "output": EMPTY_ITEMS})["metrics"][0]
        assert empty_items["first_output_item"]["present"] and not empty_items["first_content"]["present"]
        failed = run("/failed-before-content", code=4)["metrics"][0]
        assert failed["terminal"]["present"] and not failed["terminal_completed"]
        assert not failed["first_content"]["present"]
        phases = run("/phases")["metrics"][0]
        marks = [phases[name]["us"] for name in ("first_byte", "first_content", "first_output_item", "terminal")]
        assert all(phases[name]["present"] for name in ("first_byte", "first_content", "first_output_item", "terminal"))
        assert marks == sorted(marks), phases
        for path in ("/hang", "/backoff"):
            value = run(path, code=5, cancel=120)
            assert 100 <= value["elapsed"] < 600, (path, value)
            assert not value["metrics"][-1]["first_content"]["present"]
            assert not value["metrics"][-1]["terminal"]["present"]
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
