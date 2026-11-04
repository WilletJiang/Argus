"""Offline contracts for the long-run evaluator; never use real credentials."""

import asyncio, os, pathlib, tempfile, unittest
from endurance_task import prepare, protected_hashes, PROMPT, SPEC
from endurance_runner import StartBarrier, run_harness, summarize
from endurance_codex import run_codex
from unittest.mock import patch

MOCK = r"""#!/usr/bin/env python3
import json,sys,time,threading
lock=threading.Lock(); started=False; hang=False

def emit(value):
    with lock: print(json.dumps(value),flush=True)
if 'app-server' not in sys.argv:
    prompt=sys.stdin.read()
    emit({'event':'model.completed','data':{'model':'gpt-6.1-terra','usage':{'input_tokens':100,'output_tokens':20,'input_tokens_details':{'cached_tokens':40},'output_tokens_details':{'reasoning_tokens':8}}}})
    if 'HANG' in prompt: time.sleep(20)
    emit({'model':'gpt-6.1-terra','state':'completed','model_calls':1,'tool_calls':0})
else:
    for line in sys.stdin:
        q=json.loads(line); method=q.get('method'); id=q.get('id')
        if method=='initialize': emit({'id':id,'result':{}})
        elif method=='thread/start': emit({'id':id,'result':{'thread':{'id':'mock-thread'},'model':q['params']['model']}})
        elif method=='turn/start':
            assert q['params']['effort']=='xhigh'
            hang='HANG' in q['params']['input'][0]['text']
            emit({'id':id,'result':{'turn':{'id':'mock-turn'}}})
            emit({'method':'thread/tokenUsage/updated','params':{'tokenUsage':{'total':{'inputTokens':100,'cachedInputTokens':40,'outputTokens':20,'reasoningOutputTokens':8,'totalTokens':120}}}})
            if not hang: emit({'method':'turn/completed','params':{'turn':{'status':'completed'}}})
        elif method=='turn/interrupt':
            emit({'id':id,'result':{}}); emit({'method':'turn/completed','params':{'turn':{'status':'interrupted'}}})
"""


class EnduranceChecks(unittest.TestCase):
    def test_persistence_audit_handles_list_and_map_layouts(self):
        from endurance_quality import QualityChecks

        for jobs in ([{"id": "a", "priority": 0}], {"a": {"id": "a", "priority": 0}}):
            document = {"jobs": jobs}
            probe = QualityChecks("test_history_matches_current_job")
            probe.create = lambda: None
            probe.corrupt = lambda mutate: mutate(document)
            probe.test_history_matches_current_job()
            job = jobs["a"] if isinstance(jobs, dict) else jobs[0]
            self.assertEqual(job["priority"], 99)

    def test_fixture_is_reproducible_and_blind(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = pathlib.Path(tmp) / "a"
            b = pathlib.Path(tmp) / "b"
            self.assertEqual(prepare(a), prepare(b))
            self.assertEqual(protected_hashes(a), protected_hashes(b))
            for term in ("600 seconds", "10 minutes", "time limit", "benchmark"):
                self.assertNotIn(term, (PROMPT + SPEC).lower())

    def test_start_barrier_cohort_and_abort(self):
        async def check():
            barrier = StartBarrier(6)
            entered = []

            async def worker(index):
                await barrier.wait()
                entered.append(index)

            tasks = [asyncio.create_task(worker(i)) for i in range(5)]
            await asyncio.sleep(0)
            self.assertEqual(entered, [])
            tasks.append(asyncio.create_task(worker(5)))
            await asyncio.gather(*tasks)
            self.assertEqual(sorted(entered), list(range(6)))
            broken = StartBarrier(2)
            waiter = asyncio.create_task(broken.wait())
            await asyncio.sleep(0)
            await broken.abort()
            with self.assertRaises(RuntimeError):
                await waiter

        asyncio.run(check())

    def test_usage_subsets_and_cumulative_codex(self):
        c = {"runner": "codex", "timed_out": False, "state": "completed", "events": []}
        for n in (100, 300, 300):
            c["events"].append(
                {
                    "seconds": 1,
                    "event": {
                        "method": "thread/tokenUsage/updated",
                        "params": {
                            "tokenUsage": {
                                "total": {
                                    "inputTokens": n,
                                    "outputTokens": 20,
                                    "cachedInputTokens": 40,
                                    "reasoningOutputTokens": 8,
                                }
                            }
                        },
                    },
                }
            )
        s = summarize(c)
        self.assertEqual(s["usage"]["input_tokens"], 300)
        self.assertEqual(s["confirmed_model_responses"], 2)
        self.assertEqual(s["usage"]["output_tokens"], 20)

    def test_explicit_probe_credential_overrides_parent(self):
        mock_source = MOCK.replace(
            "else:\n    for line in sys.stdin:",
            "else:\n    import os\n"
            "    assert os.environ['ACCESS_TOKEN'] == 'offline-override'\n"
            "    assert 'AH_ACCESS_TOKEN' not in os.environ\n"
            "    for line in sys.stdin:",
        )
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ):
            root = pathlib.Path(tmp)
            mock = root / "mock"
            mock.write_text(mock_source)
            mock.chmod(0o700)
            for inherited in (None, "different-parent-fixture"):
                if inherited is None:
                    os.environ.pop("AH_ACCESS_TOKEN", None)
                else:
                    os.environ["AH_ACCESS_TOKEN"] = inherited
                directory = root / str(inherited)
                (directory / "workspace").mkdir(parents=True)
                result = asyncio.run(
                    run_codex(
                        directory,
                        str(mock),
                        "gpt-6.1-terra",
                        "xhigh",
                        "READY",
                        2,
                        access_token="offline-override",
                    )
                )
                self.assertEqual(result["state"], "completed")

    def test_adapters_completion_and_external_cutoff(self):
        previous = os.environ.get("AH_ACCESS_TOKEN")
        os.environ["AH_ACCESS_TOKEN"] = "offline-fake-token"
        try:
            with tempfile.TemporaryDirectory() as tmp:
                root = pathlib.Path(tmp)
                mock = root / "mock"
                mock.write_text(MOCK)
                mock.chmod(0o700)

                async def check():
                    for hang in (False, True):
                        barrier = StartBarrier(2)
                        dirs = []
                        for name in ("harness", "codex"):
                            d = root / (name + str(hang))
                            (d / "workspace").mkdir(parents=True)
                            dirs.append(d)
                        prompt = "HANG" if hang else "READY"
                        results = await asyncio.gather(
                            run_harness(
                                dirs[0],
                                mock,
                                str(mock),
                                "gpt-6.1-terra",
                                "xhigh",
                                prompt,
                                1,
                                barrier,
                            ),
                            run_codex(
                                dirs[1],
                                str(mock),
                                "gpt-6.1-terra",
                                "xhigh",
                                prompt,
                                1,
                                barrier,
                            ),
                        )
                        for result in results:
                            result = summarize(result)
                            self.assertEqual(result["timed_out"], hang)
                            self.assertEqual(result["usage"]["input_tokens"], 100)
                            self.assertEqual(result["usage"]["cached_input_tokens"], 40)
                            self.assertEqual(
                                result["usage"]["reasoning_output_tokens"], 8
                            )
                            self.assertEqual(result["usage_complete"], not hang)
                            self.assertLess(result["seconds"], 7)

                asyncio.run(check())
        finally:
            if previous is None:
                os.environ.pop("AH_ACCESS_TOKEN", None)
            else:
                os.environ["AH_ACCESS_TOKEN"] = previous


if __name__ == "__main__":
    unittest.main()
