"""Exercise RPC concurrency, strict framing, offline metadata, and shutdown."""
import http.server
import json
import os
import pathlib
import queue
import signal
import subprocess
import sys
import tempfile
import threading
import time

CLI = sys.argv[1]
SECRET = "rpc-fixture-not-a-credential"
entered = queue.Queue()
release = threading.Event()


class Service(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        self.rfile.read(int(self.headers["Content-Length"]))
        initial = b'data: {"type":"response.created","response":{"status":"in_progress"}}\n\n'
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(initial) + 1000))
        self.end_headers()
        try:
            self.wfile.write(initial)
            self.wfile.flush()
            entered.put(True)
            release.wait(5)
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True


class Client:
    def __init__(self, db, endpoint, token=True):
        env = dict(os.environ)
        env.pop("AH_ACCESS_TOKEN", None)
        if token:
            env["AH_ACCESS_TOKEN"] = SECRET
        self.process = subprocess.Popen(
            [CLI, "rpc", "--db", str(db), "--endpoint", endpoint, "--allow-http-loopback"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1, env=env)
        self.messages = queue.Queue()
        self.output = []
        self.pending = []
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        for line in self.process.stdout:
            self.output.append(line)
            self.messages.put(json.loads(line))

    def send(self, value):
        self.raw(json.dumps(value) + "\n")

    def raw(self, value):
        self.process.stdin.write(value)
        self.process.stdin.flush()

    def receive(self, predicate, timeout=5):
        for i, message in enumerate(self.pending):
            if predicate(message):
                return self.pending.pop(i)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            message = self.messages.get(timeout=max(0.001, deadline - time.monotonic()))
            if predicate(message):
                return message
            self.pending.append(message)
        raise AssertionError("RPC reply timed out")

    def reply(self, ident):
        return self.receive(lambda message: message.get("id") == ident and "status" in message)

    def request(self, ident, method, **fields):
        self.send(dict(id=ident, method=method, **fields))
        return self.reply(ident)

    def close(self):
        if not self.process.stdin.closed:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
        self.reader.join(timeout=2)
        stderr = self.process.stderr.read()
        assert self.process.returncode == 0, stderr
        assert SECRET not in "".join(self.output) + stderr
        self.process.stdout.close()
        self.process.stderr.close()


def start_run(client, label):
    created = client.request(label + "-create", "create", model="local-fixture")
    assert created["status"] == "ok", created
    session = created["result"]["id"]
    submitted = client.request(label + "-submit", "submit", session=session, command="u1", text="wait")
    assert submitted["status"] == "ok", submitted
    client.send(dict(id=label + "-run", method="run", session=session))
    entered.get(timeout=3)
    return session


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Service)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    endpoint = f"http://127.0.0.1:{server.server_port}/responses"
    try:
        with tempfile.TemporaryDirectory(prefix="ah-rpc-") as tmp:
            root = pathlib.Path(tmp)
            client = Client(root / "offline.db", endpoint, token=False)
            try:
                assert client.request("list", "list")["status"] == "ok"
                offline = client.request("create", "create", model="offline")
                assert offline["status"] == "ok"
                assert client.request("inspect", "inspect", session=offline["result"]["id"])["status"] == "ok"
                for frame in ("{broken}\n", "[]\n", '{"method":"list\\u0000suffix"}\n',
                              '{"method":"list","method":"cancel"}\n',
                              '{"method":"list","data":' + "[" * 100 + "0" + "]" * 100 + "}\n",
                              "x" * (1024 * 1024 + 1) + "\n", '{"method":"list"}\x00\n'):
                    client.raw(frame)
                    response = client.receive(lambda message: "status" in message and "id" not in message)
                    assert response["status"] == "invalid", (frame[:100], response)
                assert client.request("after-invalid", "list")["status"] == "ok"
                client.raw('{"id":"split","method":"li')
                client.raw('st"}\n')
                assert client.reply("split")["status"] == "ok"
                client.process.send_signal(signal.SIGINT)
                assert client.request("idle-sigint", "list")["status"] == "ok"
            finally:
                client.close()
            assert entered.empty(), "Metadata commands unexpectedly called the provider"

            client = Client(root / "cancel.db", endpoint)
            try:
                start_run(client, "cancel")
                assert client.request("busy", "list")["status"] == "busy"
                begin = time.monotonic()
                assert client.request("cancel-ack", "cancel")["status"] == "ok"
                response = client.reply("cancel-run")
                assert response["status"] == "cancelled", response
                assert time.monotonic() - begin < 2
                assert response["result"]["state"] == "stopped", response
            finally:
                client.close()

            client = Client(root / "signal.db", endpoint)
            try:
                start_run(client, "signal")
                client.process.send_signal(signal.SIGINT)
                response = client.reply("signal-run")
                assert response["status"] == "cancelled", response
                assert client.request("after-sigint", "list")["status"] in ("ok", "busy")
            finally:
                client.close()

            client = Client(root / "idle-term.db", endpoint, token=False)
            try:
                assert client.request("ready", "list")["status"] == "ok"
                begin = time.monotonic()
                client.process.send_signal(signal.SIGTERM)
                client.process.wait(timeout=2)  # Keep stdin open: SIGTERM must wake idle RPC itself.
                assert time.monotonic() - begin < 2
            finally:
                client.close()

            client = Client(root / "active-term.db", endpoint)
            try:
                start_run(client, "term")
                begin = time.monotonic()
                client.process.send_signal(signal.SIGTERM)
                client.process.wait(timeout=2)
                response = client.reply("term-run")
                assert response["status"] == "cancelled", response
                assert response["result"]["state"] == "stopped", response
                assert time.monotonic() - begin < 2
            finally:
                client.close()

            client = Client(root / "partial.db", endpoint, token=False)
            assert client.request("ready", "list")["status"] == "ok"
            client.raw('{"id":"unterminated","method":"list"}')
            client.close()
            replies = [json.loads(line) for line in client.output]
            assert not any(value.get("id") == "unterminated" for value in replies)
            assert any(value.get("status") == "invalid" and "EOF" in value.get("error", "") for value in replies)

            client = Client(root / "eof.db", endpoint)
            start_run(client, "eof")
            begin = time.monotonic()
            client.close()
            assert time.monotonic() - begin < 2
            replies = [json.loads(line) for line in client.output]
            assert any(value.get("id") == "eof-run" and value.get("status") == "cancelled" for value in replies)
            for saved in root.iterdir():
                if saved.is_file():
                    assert SECRET.encode() not in saved.read_bytes(), saved
        print("RPC passed: strict frames, offline metadata, busy, cancel, SIGINT, idle/active SIGTERM, EOF cleanup, no token leakage")
    finally:
        release.set()
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
