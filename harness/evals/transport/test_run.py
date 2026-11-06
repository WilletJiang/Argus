"""Offline success barriers: transport completion and expected negative controls."""

import copy
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock, Mock, patch
import uuid

from .run import accepted, previous_response_rejected, probe
from .workload import Workload


def completed():
    return {"status": "completed", "model": "m", "effort": "xhigh", "service_tier": "default",
            "store": False, "cache_key_matches": True}


def tool_response(cursor):
    return {"id": f"response-{cursor}", "output": [{
        "type": "function_call", "namespace": "local", "name": "advance",
        "arguments": '{"cursor":' + str(cursor) + '}', "call_id": f"call-{cursor}",
    }]}


class AcceptanceTests(unittest.TestCase):
    def test_late_error_blocks_tool_dispatch(self):
        for failure in ({"transport_error": "ClientPayloadError"},
                        {"error": {"type": "server_error"}}):
            with self.subTest(failure=failure):
                work = Workload("m", "xhigh", "seed", "key")
                original = copy.deepcopy(work.history)
                row = {**completed(), **failure}
                self.assertFalse(accepted(row, tool_response(0), work))
                self.assertFalse(row["transport_complete"])
                self.assertEqual(row["tool_effects"], 0)
                self.assertEqual(work.history, original)

    def test_clean_completion_can_dispatch_tool(self):
        work = Workload("m", "xhigh", "seed", "key")
        row = completed()
        self.assertTrue(accepted(row, tool_response(0), work))
        self.assertTrue(row["transport_complete"])
        self.assertEqual(row["tool_effects"], 1)

    def test_configuration_controls_fail_closed(self):
        for field, value in (("store", True), ("store", None),
                             ("cache_key_matches", False), ("cache_key_matches", None)):
            with self.subTest(field=field, value=value):
                work = Workload("m", "xhigh", "seed", "key")
                row = completed()
                if value is None:
                    row.pop(field)
                else:
                    row[field] = value
                self.assertFalse(accepted(row, tool_response(0), work))
                self.assertFalse(row["configuration_matches"])
                self.assertEqual(row["tool_effects"], 0)

    def test_negative_control_requires_relevant_rejection(self):
        expected = {"status": "error", "error": {"type": "invalid_request_error",
                                                   "code": "previous_response_not_found"}}
        self.assertTrue(previous_response_rejected(expected))
        self.assertTrue(previous_response_rejected({"status": "error", "error": {
            "type": "invalid_request_error", "mentions_previous_response": True}}))
        for row in (
            completed(), {"status": "interrupted"},
            {**expected, "transport_error": "ConnectionResetError"},
            {"status": "error", "error": {"type": "invalid_request_error", "code": "invalid_model"}},
            {"status": "error", "error": {"type": "server_error", "mentions_previous_response": True}},
        ):
            with self.subTest(row=row):
                self.assertFalse(previous_response_rejected(row))


class ProbeTests(unittest.IsolatedAsyncioTestCase):
    async def test_successful_recovery_cannot_mask_failed_negative_control(self):
        identifier = uuid.UUID(int=0)
        rejected = {"status": "error", "error": {
            "type": "invalid_request_error", "mentions_previous_response": True}}
        for negative, expected in ((rejected, True), (completed(), False),
                                   ({"status": "interrupted"}, False)):
            with self.subTest(negative=negative):
                client = Mock(open_ms=1)
                client.__aenter__ = AsyncMock(return_value=client)
                client.__aexit__ = AsyncMock(return_value=False)
                client.request = AsyncMock(side_effect=[
                    (completed(), tool_response(0)),
                    (completed(), tool_response(1)),
                    (copy.deepcopy(negative), tool_response(2)),
                    (completed(), {"id": "recovered", "output": [{"type": "message", "content": [
                        {"type": "output_text", "text": f"DONE {identifier.hex} 3"}]}]}),
                ])
                ledger = SimpleNamespace(data={"probe": []}, save=Mock())
                args = SimpleNamespace(model="m", effort="xhigh")
                with patch("evals.transport.run.Client", return_value=client), \
                     patch("evals.transport.run.uuid.uuid4", return_value=identifier), \
                     patch("builtins.print"):
                    result = await probe("dummy-test-token", args, ledger)
                self.assertEqual(result, expected)
                self.assertEqual(ledger.data["probe_passed"], expected)
                self.assertEqual(client.request.await_count, 4)
                self.assertFalse(ledger.data["probe"][2]["tools_dispatched"])
                self.assertTrue(ledger.data["probe"][3]["content_valid"])
                self.assertEqual(ledger.data["probe"][3]["tool_effects"], 2)
                self.assertEqual(ledger.data["probe"][3]["repeated_tool_effects"], 0)


if __name__ == "__main__":
    unittest.main()
