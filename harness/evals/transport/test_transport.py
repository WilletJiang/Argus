"""Offline checks for content-safe receipts and replay/effect boundaries."""

import json
import os
import unittest
from unittest.mock import patch

from aiohttp import web

from .client import Client, Receipt
from .workload import Workload


def call(cursor, identifier):
    return {"type": "function_call", "namespace": "local", "name": "advance",
            "arguments": '{"cursor":' + str(cursor) + '}', "call_id": identifier}


class TransportTests(unittest.TestCase):
    def test_empty_completed_items_do_not_set_first_content(self):
        receipt = Receipt({"input": []}, 2)
        empty = [
            {"type": "message"},
            {"type": "message", "content": []},
            {"type": "message", "content": [{"type": "output_text", "text": ""}]},
            {"type": "function_call", "arguments": ""},
            {"type": "custom_tool_call", "input": ""},
            {"type": "reasoning", "summary": [{"type": "summary_text", "text": "thinking"}]},
        ]
        for index, item in enumerate(empty):
            with self.subTest(item=item):
                receipt.event({"type": "response.output_item.done", "item": item, "output_index": index})
                self.assertIsNone(receipt.row["first_content_ms"])
                self.assertIsNone(receipt.row["first_content_source"])
        receipt.event({"type": "response.output_text.delta", "delta": ""})
        self.assertIsNone(receipt.row["first_content_ms"])
        self.assertEqual(receipt.row["nonempty_content_deltas"], 0)
        receipt.event({"type": "response.output_text.delta", "delta": "OK"})
        self.assertIsNotNone(receipt.row["first_content_ms"])
        self.assertEqual(receipt.row["first_content_source"], "response.output_text.delta")
        self.assertEqual(receipt.row["nonempty_content_deltas"], 1)

    def test_nonempty_completed_content_is_a_fallback(self):
        items = [
            {"type": "message", "content": [{"type": "output_text", "text": "OK"}]},
            {"type": "function_call", "arguments": "{}"},
            {"type": "custom_tool_call", "input": "print(1)"},
        ]
        for item in items:
            with self.subTest(item=item):
                receipt = Receipt({"input": []}, 2)
                receipt.event({"type": "response.output_item.done", "item": item, "output_index": 0})
                first = receipt.row["first_content_ms"]
                self.assertIsNotNone(first)
                self.assertEqual(receipt.row["first_content_source"], "response.output_item.done:" + item["type"])
                receipt.event({"type": "response.output_text.delta", "delta": "later"})
                self.assertEqual(receipt.row["first_content_ms"], first)
                self.assertEqual(receipt.row["nonempty_content_deltas"], 1)

    def test_incremental_only_sends_new_receipt(self):
        work = Workload("m", "xhigh", "seed", "key", steps=2)
        self.assertTrue(work.accept({"output": [call(0, "first")]}))
        incremental = work.request("response-a")
        self.assertEqual(incremental["previous_response_id"], "response-a")
        self.assertEqual([i["type"] for i in incremental["input"]], ["function_call_output"])
        self.assertEqual(len(work.request()["input"]), 3)
        self.assertNotIn("previous_response_id", work.request())

    def test_duplicate_or_wrong_cursor_does_not_execute(self):
        for second in (call(0, "second"), call(1, "first")):
            work = Workload("m", "xhigh", "seed", "key", steps=2)
            self.assertTrue(work.accept({"output": [call(0, "first")]}))
            self.assertFalse(work.accept({"output": [second]}))
            self.assertEqual(len(work.executed), 1)

    def test_full_replay_and_final_do_not_repeat_effects(self):
        work = Workload("m", "xhigh", "seed", "key", steps=2)
        for i in range(2):
            self.assertTrue(work.accept({"output": [call(i, str(i))]}))
        replay = work.request()
        self.assertEqual(len(replay["input"]), 5)
        self.assertEqual(replay["tool_choice"], "none")
        self.assertTrue(work.accept({"output": [{"type": "message", "content": [
            {"type": "output_text", "text": "DONE seed 3"}]}]}))
        self.assertEqual(len(work.executed), 2)

    def test_empty_terminal_output_and_usage_preserved(self):
        receipt = Receipt({"input": [], "prompt_cache_key": "key"}, 12)
        receipt.event({"type": "response.created", "response": {"id": "response-a"}})
        self.assertIsNone(receipt.row["first_content_ms"])
        item = call(0, "first")
        receipt.event({"type": "response.output_item.done", "item": item, "output_index": 0})
        usage = {"input_tokens": 9, "input_tokens_details": {"cached_tokens": 4}, "output_tokens": 2}
        receipt.event({"type": "response.completed", "response": {
            "id": "response-a", "status": "completed", "output": [], "usage": usage,
            "prompt_cache_key": "key"}})
        row, response = receipt.finish()
        self.assertEqual(response["output"], [item])
        self.assertEqual(row["usage"], usage)
        self.assertTrue(row["cache_key_matches"])
        self.assertNotIn("output", row)
        self.assertNotIn("response-a", str(row))

    def test_missing_usage_stays_unknown(self):
        receipt = Receipt({"input": []}, 2)
        receipt.event({"type": "error", "error": {"type": "invalid_request_error",
                                                   "message": "previous response missing"}})
        row, _ = receipt.finish()
        self.assertIsNone(row["usage"])
        self.assertTrue(row["error"]["mentions_previous_response"])
        self.assertNotIn("message", row["error"])


class LocalTransportTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.requests = []
        self.connections = []
        app = web.Application()
        app.router.add_route("*", "/v1/responses", self.handle)
        self.runner = web.AppRunner(app)
        await self.runner.setup()
        site = web.TCPSite(self.runner, "127.0.0.1", 0)
        await site.start()
        port = self.runner.addresses[0][1]
        self.endpoint = patch("evals.transport.client.ENDPOINT", f"http://127.0.0.1:{port}/v1/responses")
        self.endpoint.start()
        self.environment = patch.dict(os.environ, {}, clear=True)
        self.environment.start()

    async def asyncTearDown(self):
        self.environment.stop()
        self.endpoint.stop()
        await self.runner.cleanup()

    def events(self, body):
        self.requests.append(body)
        return [
            {"type": "response.output_text.delta", "delta": "OK"},
            {"type": "response.output_item.done", "output_index": 0,
             "item": {"type": "message", "content": [{"type": "output_text", "text": "OK"}]}},
            {"type": "response.completed", "response": {"id": "r", "status": "completed",
             "output": [], "usage": {"input_tokens": 1, "output_tokens": 1}}},
        ]

    async def handle(self, request):
        self.assertEqual(request.headers["session-id"], "key")
        self.assertEqual(request.headers["Authorization"], "Bearer dummy-test-token")
        self.connections.append(request.transport)
        if request.headers.get("Upgrade", "").lower() == "websocket":
            socket = web.WebSocketResponse()
            await socket.prepare(request)
            async for message in socket:
                for event in self.events(json.loads(message.data)):
                    await socket.send_json(event)
            return socket
        response = web.StreamResponse(headers={"Content-Type": "text/event-stream"})
        await response.prepare(request)
        for event in self.events(await request.json()):
            await response.write(("data: " + json.dumps(event) + "\n\n").encode())
        await response.write(b"data: [DONE]\n\n")
        await response.write_eof()
        return response

    async def test_http_reuses_connection_and_consumes_terminal(self):
        rows = []
        async with Client("dummy-test-token", "key", False) as client:
            for _ in range(2):
                row, response = await client.request({"input": []})
                rows.append(row)
                self.assertEqual(response["output"][0]["content"][0]["text"], "OK")
                self.assertEqual(row["status"], "completed")
        self.assertTrue(rows[0]["connection_created"])
        self.assertTrue(rows[1]["connection_reused"])
        self.assertIs(self.connections[0], self.connections[1])
        self.assertTrue(all(body["stream"] for body in self.requests))

    async def test_websocket_sends_incremental_and_omits_stream(self):
        async with Client("dummy-test-token", "key", True) as client:
            await client.request({"input": [{"role": "user", "content": "start"}]})
            row, _ = await client.request({"input": [], "previous_response_id": "r"})
        self.assertEqual(len(self.connections), 1)
        self.assertEqual(self.requests[1]["previous_response_id"], "r")
        self.assertEqual(self.requests[1]["input"], [])
        self.assertNotIn("stream", self.requests[1])
        self.assertEqual(self.requests[1]["type"], "response.create")
        self.assertIsNotNone(row["first_content_ms"])
        self.assertEqual(row["first_content_source"], "response.output_text.delta")
        self.assertEqual(row["nonempty_content_deltas"], 1)


if __name__ == "__main__":
    unittest.main()
