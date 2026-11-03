"""Verify the installed Codex emits the requested model/effort before paid work."""

import http.server
import json
import pathlib
import tempfile
import threading
from endurance_codex import run_codex


async def verify(codex, model, effort):
    captured = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            captured.append(
                {k: body.get(k) for k in ("model", "reasoning", "store", "stream")}
            )
            item = {
                "id": "msg_probe",
                "type": "message",
                "role": "assistant",
                "status": "completed",
                "content": [
                    {"type": "output_text", "text": "READY", "annotations": []}
                ],
            }
            response = {
                "id": "resp_probe",
                "object": "response",
                "model": model,
                "status": "completed",
                "output": [item],
                "usage": {"input_tokens": 1, "output_tokens": 1, "total_tokens": 2},
            }
            events = [
                {
                    "type": "response.created",
                    "response": {
                        "id": "resp_probe",
                        "status": "in_progress",
                        "output": [],
                    },
                },
                {
                    "type": "response.output_item.added",
                    "output_index": 0,
                    "item": dict(item, status="in_progress", content=[]),
                },
                {
                    "type": "response.output_text.delta",
                    "item_id": "msg_probe",
                    "output_index": 0,
                    "content_index": 0,
                    "delta": "READY",
                },
                {"type": "response.output_item.done", "output_index": 0, "item": item},
                {"type": "response.completed", "response": response},
            ]
            data = "".join("data: " + json.dumps(e) + "\n\n" for e in events).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="ah-codex-wire-") as tmp:
            d = pathlib.Path(tmp)
            (d / "workspace").mkdir()
            result = await run_codex(
                d,
                codex,
                model,
                effort,
                "Reply READY without tools.",
                20,
                base_url=f"http://127.0.0.1:{server.server_port}/v1",
                access_token="offline-wire-probe",
            )
            if result["state"] != "completed" or len(captured) != 1:
                raise RuntimeError("Codex wire probe did not complete")
            request = captured[0]
            if (
                request["model"] != model
                or (request.get("reasoning") or {}).get("effort") != effort
                or request["store"] is not False
                or request["stream"] is not True
            ):
                raise RuntimeError(
                    "Codex wire request differs from requested model/reasoning/transport"
                )
            return request
    finally:
        server.shutdown()
        server.server_close()
