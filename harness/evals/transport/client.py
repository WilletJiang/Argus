"""Controlled HTTP/SSE and WebSocket clients; content and credentials stay in memory."""

import asyncio
from collections import Counter
import hashlib
import json
import os
import time

import aiohttp


ENDPOINT = "https://api.openai.com/v1/responses"
TERMINAL = {"response.completed", "response.failed", "response.incomplete", "error"}
GENERATED = {
    "response.output_text.delta",
    "response.function_call_arguments.delta",
    "response.custom_tool_call_input.delta",
}


def encode(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode()


def digest(value):
    return hashlib.sha256(encode(value)).hexdigest()


def has_content(item):
    kind = item.get("type")
    if kind == "message":
        return any(part.get("type") == "output_text" and isinstance(part.get("text"), str)
                   and bool(part["text"]) for part in item.get("content") or [])
    field = {"function_call": "arguments", "custom_tool_call": "input"}.get(kind)
    value = item.get(field) if field else None
    return isinstance(value, str) and bool(value)


class Receipt:
    def __init__(self, body, size):
        self.started = time.perf_counter()
        self.response = {}
        self.key = body.get("prompt_cache_key")
        self.items = {}
        self.events = Counter()
        self.row = {
            "request_bytes": size,
            "input_items": len(body["input"]),
            "input_sha256": digest(body["input"]),
            "incremental": bool(body.get("previous_response_id")),
            "first_event_ms": None,
            "first_content_ms": None,
            "first_content_source": None,
            "nonempty_content_deltas": 0,
            "completed_ms": None,
            "status": "interrupted",
            "usage": None,
        }

    def elapsed(self):
        return round((time.perf_counter() - self.started) * 1000, 3)

    def event(self, event):
        kind = event.get("type", "unknown")
        self.events[kind] += 1
        if self.row["first_event_ms"] is None:
            self.row["first_event_ms"] = self.elapsed()
        delta = event.get("delta")
        if kind in GENERATED and isinstance(delta, str) and delta:
            self.row["nonempty_content_deltas"] += 1
            if self.row["first_content_ms"] is None:
                self.row["first_content_ms"] = self.elapsed()
                self.row["first_content_source"] = kind
        if kind == "response.output_item.done":
            item = event["item"]
            self.items[event.get("output_index", len(self.items))] = item
            if self.row["first_content_ms"] is None and has_content(item):
                self.row["first_content_ms"] = self.elapsed()
                self.row["first_content_source"] = kind + ":" + item["type"]
        if kind not in TERMINAL:
            return False
        self.response = event.get("response", {})
        self.row.update(
            status=self.response.get("status", "error"),
            completed_ms=self.elapsed(),
            usage=self.response.get("usage"),
            model=self.response.get("model"),
            effort=(self.response.get("reasoning") or {}).get("effort"),
            service_tier=self.response.get("service_tier"),
            response_id_sha256=digest(self.response.get("id")),
            terminal_output_items=len(self.response.get("output") or []),
            returned_cache_key=self.response.get("prompt_cache_key"),
            cache_key_matches=self.response.get("prompt_cache_key") == self.key,
            store=self.response.get("store"),
        )
        error = event.get("error") or self.response.get("error")
        if error:
            # Only machine-readable fields; descriptions can contain echoed input.
            self.row["error"] = {k: error.get(k) for k in ("code", "type", "param")}
            description = error.get("message", "")
            self.row["error"]["mentions_previous_response"] = "previous" in description.lower() and "response" in description.lower()
            self.row["error"]["message_sha256"] = digest(description)
        return True

    def finish(self):
        outputs = self.response.get("output") or [self.items[k] for k in sorted(self.items)]
        self.response["output"] = outputs
        self.row.update(
            elapsed_ms=self.elapsed(),
            events=dict(self.events),
            streamed_output_items=len(self.items),
            output_types=[item.get("type") for item in outputs],
            output_sha256=digest(outputs),
        )
        return self.row, self.response


class Client:
    def __init__(self, token, key, websocket):
        self.headers = {"Authorization": "Bearer " + token, "session-id": key}
        self.websocket = websocket
        self.socket = None
        self.trace = {}
        self.open_ms = None

    async def __aenter__(self):
        trace = aiohttp.TraceConfig()

        async def starting(session, context, params):
            self.trace["connection_started"] = time.perf_counter()
            self.trace["connection_created"] = True

        async def connected(session, context, params):
            self.trace["connection_ms"] = round(
                (time.perf_counter() - self.trace.pop("connection_started")) * 1000, 3
            )

        async def reused(session, context, params):
            self.trace["connection_reused"] = True

        trace.on_connection_create_start.append(starting)
        trace.on_connection_create_end.append(connected)
        trace.on_connection_reuseconn.append(reused)
        self.session = aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=60),
            connector=aiohttp.TCPConnector(limit=1),
            trace_configs=[trace], trust_env=True,
        )
        try:
            if self.websocket:
                started = time.perf_counter()
                self.socket = await self.session.ws_connect(
                    ENDPOINT.replace("https:", "wss:"), headers=self.headers,
                    proxy=os.environ.get("HTTPS_PROXY") or os.environ.get("https_proxy"),
                    timeout=aiohttp.ClientWSTimeout(ws_receive=60, ws_close=5),
                    compress=0, max_msg_size=16 * 1024 * 1024,
                )
                self.open_ms = round((time.perf_counter() - started) * 1000, 3)
        except Exception:
            await self.session.close()
            raise
        return self

    async def __aexit__(self, *args):
        if self.socket is not None:
            await self.socket.close()
        await self.session.close()

    async def request(self, body):
        wire = dict(body)
        if self.websocket:
            wire["type"] = "response.create"
        else:
            wire["stream"] = True
        payload = encode(wire)
        receipt = Receipt(body, len(payload))
        self.trace = {"connection_created": False, "connection_reused": False}
        try:
            async with asyncio.timeout(60):
                if self.websocket:
                    await self.socket.send_str(payload.decode())
                    async for message in self.socket:
                        if message.type != aiohttp.WSMsgType.TEXT:
                            raise RuntimeError("non_text_websocket_event")
                        if receipt.event(json.loads(message.data)):
                            break
                else:
                    async with self.session.post(
                        ENDPOINT, data=payload,
                        headers={**self.headers, "Content-Type": "application/json",
                                 "Accept": "text/event-stream"},
                    ) as response:
                        receipt.row["http_status"] = response.status
                        receipt.row["http_version"] = str(response.version)
                        receipt.row["returned_session_id"] = response.headers.get("session-id")
                        if response.status != 200:
                            receipt.row["status"] = "http_error"
                            await response.read()
                        else:
                            data = []
                            async for line in response.content:
                                line = line.decode().rstrip("\r\n")
                                if line.startswith("data:"):
                                    data.append(line[5:].lstrip())
                                elif not line and data:
                                    value = "\n".join(data)
                                    data = []
                                    if value != "[DONE]":
                                        receipt.event(json.loads(value))
        except Exception as error:
            receipt.row["transport_error"] = type(error).__name__
        receipt.row.update({k: v for k, v in self.trace.items() if k != "connection_started"})
        return receipt.finish()
