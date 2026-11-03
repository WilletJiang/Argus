"""Offline checks for cache telemetry and the controlled workload."""

import copy
import json
import unittest
import tempfile
from pathlib import Path
from endurance_runner import Trace
from unittest.mock import patch
from types import SimpleNamespace
import cache_study
from cache_trace import CacheTrace
from cache_workload import request_body, grow


class CacheChecks(unittest.TestCase):
    def test_append_fingerprints_and_no_prompt_content(self):
        trace = CacheTrace()
        body = {
            "model": "mock",
            "instructions": "private instructions marker",
            "input": [{"role": "user", "content": "private user marker"}],
            "tools": [],
            "prompt_cache_key": "private key marker",
        }
        first = trace.request(body)
        body = copy.deepcopy(body)
        body["input"].append(
            {
                "type": "function_call_output",
                "call_id": "x",
                "output": "private result marker",
            }
        )
        second = trace.request(body)
        self.assertEqual(second["matching_prior_items"], 1)
        self.assertFalse(second["rewritten_prior_input"])
        self.assertEqual(second["changed_fields"], [])
        for marker in (
            "private instructions marker",
            "private user marker",
            "private result marker",
            "private key marker",
        ):
            self.assertNotIn(marker, json.dumps([first, second]))
        body["input"][0]["content"] = "changed"
        self.assertTrue(trace.request(body)["rewritten_prior_input"])

    def test_trace_sink_never_persists_prepared_request(self):
        with tempfile.TemporaryDirectory() as tmp:
            trace = Trace(Path(tmp), "credential-fixture")
            trace.write(
                {
                    "event": "provider.request",
                    "data": {
                        "model": "mock",
                        "input": [{"role": "user", "content": "private prompt marker"}],
                    },
                }
            )
            trace.write(
                {
                    "event": "provider.response",
                    "elapsed_ms": 10,
                    "data": {
                        "id": "r",
                        "status": "completed",
                        "usage": {"input_tokens": 10, "output_tokens": 1},
                    },
                }
            )
            trace.close()
            for path in Path(tmp).glob("*.jsonl"):
                self.assertNotIn("private prompt marker", path.read_text())
            rows = [
                json.loads(line)
                for line in (Path(tmp) / "cache-metrics.jsonl").read_text().splitlines()
            ]
            self.assertEqual(
                [r["kind"] for r in rows], ["cache.request", "cache.response"]
            )

    def test_setting_changes_and_diagnostic_ids(self):
        trace = CacheTrace()
        body = {"model": "mock", "input": [], "tools": []}
        trace.request(body)
        body["prompt_cache_options"] = {"comparison_response_id": "resp_1"}
        self.assertEqual(trace.request(body)["changed_fields"], [])
        body["prompt_cache_options"]["comparison_response_id"] = "resp_2"
        self.assertEqual(trace.request(body)["changed_fields"], [])
        body["prompt_cache_key"] = "session"
        self.assertEqual(trace.request(body)["changed_fields"], ["prompt_cache_key"])
        body["tools"] = [{"type": "namespace", "name": "local", "tools": []}]
        self.assertEqual(trace.request(body)["changed_fields"], ["tools"])

    def test_usage_subsets_and_missing_are_not_zero(self):
        trace = CacheTrace()
        trace.request({"input": []})
        row = trace.response(
            {
                "id": "r",
                "status": "completed",
                "usage": {
                    "input_tokens": 100,
                    "output_tokens": 20,
                    "input_tokens_details": {
                        "cached_tokens": 60,
                        "cache_write_tokens": 10,
                    },
                    "output_tokens_details": {"reasoning_tokens": 8},
                    "attribution": {
                        "items": {
                            "opaque": {
                                "input_tokens": 80,
                                "cached_tokens": 60,
                                "content": [{"text": "never keep"}],
                            }
                        }
                    },
                },
                "prompt_cache_diagnostics": {
                    "type": "cache_miss",
                    "reason": "tools_changed",
                    "comparison_reusable_tokens": 90,
                    "cache_missed_tokens": 30,
                },
                "output": [{"text": "never keep"}],
            }
        )
        self.assertEqual(row["uncached_input_tokens"], 40)
        self.assertEqual(row["cache_write_tokens"], 10)
        self.assertEqual(row["output_tokens"], 20)
        self.assertEqual(row["reasoning_output_tokens"], 8)
        self.assertNotIn("never keep", json.dumps(row))
        self.assertTrue(row["diagnostics_returned"])
        missing = trace.response(None, transport_error={"code": "network"})
        self.assertIsNone(missing["input_tokens"])
        self.assertIsNone(missing["cache_write_tokens"])
        self.assertIsNone(missing["uncached_input_tokens"])
        self.assertFalse(missing["diagnostics_returned"])

    def test_study_steps_and_rejected_diagnostics(self):
        bodies = []

        class FakeClient:
            def __init__(self, driver, directory):
                pass

            def close(self):
                pass

            def request(self, body):
                bodies.append(copy.deepcopy(body))
                if body.get("prompt_cache_options") and reject[0]:
                    return {
                        "ok": False,
                        "code": "protocol",
                        "error": "provider HTTP 400",
                    }
                return {
                    "ok": True,
                    "elapsed_ms": 1,
                    "response": {
                        "id": f"resp_{len(bodies)}",
                        "model": "mock",
                        "status": "completed",
                        "reasoning": {"effort": "xhigh"},
                        "usage": {
                            "input_tokens": 100,
                            "output_tokens": 1,
                            "input_tokens_details": {"cached_tokens": 50},
                        },
                    },
                }

        args = SimpleNamespace(driver=None, model="mock", effort="xhigh")
        reject = [True]
        with (
            tempfile.TemporaryDirectory() as tmp,
            patch.object(cache_study, "Client", FakeClient),
            patch("builtins.print"),
        ):
            root = Path(tmp)
            supported = cache_study.capabilities(args, root)
            self.assertFalse(supported)
            self.assertEqual(len(bodies), 2)
            self.assertFalse(
                json.loads((root / "capabilities.json").read_text())[
                    "diagnostic_results_returned"
                ]
            )
            bodies.clear()
            reject[0] = False
            result = cache_study.run_case(
                args, root, "fixed", "instructions", True, True
            )
            self.assertEqual(len(result["responses"]), 4)
            key = bodies[0]["prompt_cache_key"]
            self.assertTrue(all(b["prompt_cache_key"] == key for b in bodies))
            self.assertEqual(bodies[0]["input"], bodies[1]["input"])
            self.assertEqual(
                bodies[2]["input"][: len(bodies[1]["input"])], bodies[1]["input"]
            )
            self.assertEqual(
                bodies[3]["input"][: len(bodies[2]["input"])], bodies[2]["input"]
            )
            self.assertEqual(
                bodies[3]["prompt_cache_options"]["comparison_response_id"], "resp_3"
            )

    def test_workload_data_and_history_are_controlled(self):
        a = request_body("mock", "xhigh", "group-a", "instructions", False)
        b = request_body("mock", "xhigh", "group-b", "instructions", True)
        self.assertEqual(a["input"], b["input"])
        self.assertEqual(a["tools"], b["tools"])
        grown = grow(a, 2)
        self.assertEqual(grown["input"][: len(a["input"])], a["input"])
        self.assertEqual(len(a["input"]), 1)
        developer = request_body("mock", "xhigh", "group-a", "developer", False)
        self.assertEqual(developer["input"][0]["content"], a["instructions"])
        self.assertEqual(developer["input"][1:], a["input"])


if __name__ == "__main__":
    unittest.main()
