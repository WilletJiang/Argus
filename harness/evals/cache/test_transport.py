"""Real C transport and request decorator against a loopback fixture only."""

import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

cache_driver, live_driver = sys.argv[1:]
received = []
routes = []


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        received.append(body)
        routes.append(self.headers.get("session-id"))
        number = len(received)
        output = []
        if body.get("model") == "tool-loop" and not any(
            x.get("type") == "function_call_output" for x in body["input"]
        ):
            output = [
                {
                    "id": "fc_read_1",
                    "type": "function_call",
                    "namespace": "local",
                    "name": "read_file",
                    "call_id": "read_1",
                    "arguments": '{"path":"file.txt"}',
                }
            ]
        response = {
            "id": f"resp_{number}",
            "model": body.get("model"),
            "status": "completed",
            "output": output,
            "usage": {
                "input_tokens": 100,
                "output_tokens": 10,
                "input_tokens_details": {"cached_tokens": 60, "cache_write_tokens": 5},
                "output_tokens_details": {"reasoning_tokens": 4},
            },
            "prompt_cache_diagnostics": {"type": "cache_hit"},
        }
        wire = (
            "".join(
                "data: "
                + json.dumps(
                    {
                        "type": "response.output_item.done",
                        "output_index": i,
                        "item": item,
                    }
                )
                + "\n\n"
                for i, item in enumerate(output)
            )
            + "data: "
            + json.dumps(
                {"type": "response.completed", "response": dict(response, output=[])}
            )
            + "\n\n"
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(wire)))
        self.end_headers()
        self.wfile.write(wire)


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
env = dict(
    os.environ,
    AH_ACCESS_TOKEN="loopback-fixture",
    AH_EVAL_ENDPOINT=f"http://127.0.0.1:{server.server_port}/responses",
    AH_EVAL_ALLOW_LOOPBACK="1",
)
try:
    body = {
        "model": "mock",
        "instructions": "stable",
        "input": [],
        "store": False,
        "stream": True,
    }
    p = subprocess.run(
        [cache_driver],
        input=json.dumps(body) + "\n" + json.dumps(body) + "\n",
        env=env,
        text=True,
        capture_output=True,
        timeout=10,
    )
    assert p.returncode == 0, p.stderr
    rows = [json.loads(line) for line in p.stdout.splitlines()]
    assert len(rows) == 2 and all(r["ok"] for r in rows)
    assert rows[1]["response"]["prompt_cache_diagnostics"] == {"type": "cache_hit"}
    assert rows[1]["response"]["usage"]["input_tokens_details"]["cached_tokens"] == 60
    assert routes == [None, None]
    # One reusable transport switches sessions and clears a previous route when omitted.
    received.clear()
    routes.clear()
    keys = ["session-a", "session-b", "session-a", None, "x" * 512]
    requests = [dict(body, prompt_cache_key=key) for key in keys]
    invalid = ["", "bad\r\nInjected: yes", "non-ascii-\u4e2d", "x" * 513]
    requests += [dict(body, prompt_cache_key=key) for key in invalid]
    p = subprocess.run(
        [cache_driver],
        input="".join(json.dumps(r) + "\n" for r in requests),
        env=env,
        text=True,
        capture_output=True,
        timeout=10,
    )
    assert p.returncode == 0, p.stderr
    rows = [json.loads(line) for line in p.stdout.splitlines()]
    assert len(rows) == len(requests)
    assert all(r["ok"] for r in rows[: len(keys)])
    assert all(not r["ok"] for r in rows[len(keys) :])
    assert routes == keys  # Unsafe keys are rejected before any HTTP request.
    for omit in (False, True):
        received.clear()
        routes.clear()
        with tempfile.TemporaryDirectory(prefix="ah-cache-adapter-") as tmp:
            root = Path(tmp)
            (root / "workspace").mkdir()
            (root / "workspace/file.txt").write_text("private fixture text")
            options = dict(
                env,
                AH_EVAL_TRACE_REQUESTS="1",
                AH_EVAL_CACHE_DIAGNOSTICS="1",
                AH_EVAL_CACHE_KEY="omit" if omit else "session",
            )
            p = subprocess.run(
                [
                    live_driver,
                    str(root / "workspace"),
                    str(root / "state.db"),
                    "tool-loop",
                    "unused",
                    "xhigh",
                    "10",
                ],
                input="Read file.txt then finish.",
                env=options,
                text=True,
                capture_output=True,
                timeout=15,
            )
            assert p.returncode == 0, p.stderr
            assert len(received) == 2
            assert ("prompt_cache_key" in received[0]) == (not omit)
            assert routes == [r.get("prompt_cache_key") for r in received]
            if not omit:
                assert (
                    received[0]["prompt_cache_key"] == received[1]["prompt_cache_key"]
                )
            assert "prompt_cache_options" not in received[0]
            assert received[1]["prompt_cache_options"] == {
                "comparison_response_id": "resp_1"
            }
            events = [json.loads(line) for line in p.stdout.splitlines()]
            assert sum(e.get("event") == "provider.request" for e in events) == 2
            assert sum(e.get("event") == "provider.response" for e in events) == 2
            tool_events = [
                e
                for e in events
                if e.get("event") in ("tool.started", "tool.completed")
            ]
            assert len(tool_events) == 2
            assert len({e["operation_id"] for e in tool_events}) == 1
            assert len({e["session_id"] for e in tool_events}) == 1
            assert received[1]["reasoning"]["effort"] == "xhigh"
    print("cache transport and final request decorator verified against loopback")
finally:
    server.shutdown()
    server.server_close()
