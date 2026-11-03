"""Post-run contract audit. These checks are separate from the frozen 33-case score."""

import importlib
import json
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch
from endurance_checks import Result


class QualityChecks(unittest.TestCase):
    def setUp(self):
        self.m = importlib.import_module("workboard")
        self.q = importlib.import_module("workboard.query")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.s = self.m.Store(self.tmp.name)
        self.addCleanup(self.s.close)

    def create(self):
        return self.s.create("a", {"x": [1]}, idempotency_key="k")

    def snapshot(self):
        return (
            self.s.list_jobs(),
            self.s.events(),
            {
                p.name: p.read_bytes()
                for p in pathlib.Path(self.tmp.name).iterdir()
                if p.is_file()
            },
        )

    def corrupt(self, mutate):
        self.s.close()
        files = list(pathlib.Path(self.tmp.name).glob("*.json"))
        self.assertEqual(len(files), 1)
        p = files[0]
        d = json.loads(p.read_text())
        mutate(d)
        p.write_text(json.dumps(d))
        raw = p.read_bytes()
        with self.assertRaises(self.m.ValidationError):
            self.m.Store(self.tmp.name)
        self.assertEqual(p.read_bytes(), raw)

    def test_historical_event_record_validation(self):
        self.create()
        self.s.claim("w", 0, 10)
        self.corrupt(
            lambda d: d["events"][0]["record"].__setitem__("state", "invented")
        )

    def test_history_matches_current_job(self):
        self.create()
        self.corrupt(lambda d: d["jobs"][0].__setitem__("priority", 99))

    def test_malformed_store_is_not_replaced(self):
        self.create()
        self.s.close()
        p = list(pathlib.Path(self.tmp.name).glob("*.json"))[0]
        p.write_text("{broken")
        with self.assertRaises(self.m.ValidationError):
            self.m.Store(self.tmp.name)
        self.assertEqual(p.read_text(), "{broken")

    def test_mutation_io_failure_rolls_back(self):
        self.create()
        before = self.snapshot()
        with patch("os.replace", side_effect=OSError("injected replacement failure")):
            with self.assertRaises(OSError):
                self.s.create("b", None, idempotency_key="new")
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.s.create("c", None, idempotency_key="new")["sequence"], 2)

    def test_transaction_io_failure_rolls_back(self):
        self.create()
        before = self.snapshot()
        with patch("os.replace", side_effect=OSError("injected replacement failure")):
            with self.assertRaises(OSError):
                with self.s.transaction():
                    self.s.cancel("a")
                    self.s.create("b", None)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.s.create("b", None)["sequence"], 2)

    def test_expired_owned_operations_do_not_reap(self):
        self.create()
        j = self.s.claim("w", 0, 10)
        before = self.snapshot()
        for call in (
            lambda: self.s.complete("a", j["lease_token"], None, 10),
            lambda: self.s.fail("a", j["lease_token"], "x", 10),
            lambda: self.s.heartbeat("a", j["lease_token"], 10, 10),
        ):
            with self.assertRaises(self.m.Conflict):
                call()
            self.assertEqual(self.snapshot(), before)

    def test_overflow_is_atomic(self):
        self.create()
        before = self.snapshot()
        with self.assertRaises(self.m.ValidationError):
            self.s.claim("w", 1e308, 1e308)
        self.assertEqual(self.snapshot(), before)

    def test_completion_validation_and_copy(self):
        self.create()
        j = self.s.claim("w", 0, 10)
        before = self.snapshot()
        for result in ((1,), {1: "x"}, [float("nan")]):
            with self.assertRaises(self.m.ValidationError):
                self.s.complete("a", j["lease_token"], result, 1)
            self.assertEqual(self.snapshot(), before)
        result = {"x": [1]}
        self.s.complete("a", j["lease_token"], result, 1)
        result["x"].append(2)
        self.assertEqual(self.s.get("a")["result"], {"x": [1]})

    def test_lease_token_not_reused_after_rollback_restart(self):
        self.create()
        with self.assertRaises(RuntimeError):
            with self.s.transaction():
                token = self.s.claim("w", 0, 10)["lease_token"]
                raise RuntimeError("abort")
        self.s.close()
        self.s = self.m.Store(self.tmp.name)
        self.addCleanup(self.s.close)
        self.assertNotEqual(self.s.claim("w", 0, 10)["lease_token"], token)

    def test_all_methods_reject_closed_store(self):
        self.create()
        self.s.close()
        self.s.close()
        calls = [
            lambda: self.s.get("a"),
            lambda: self.s.list_jobs(),
            lambda: self.s.events(),
            lambda: self.s.compact(),
            lambda: self.s.export(pathlib.Path(self.tmp.name) / "x"),
            lambda: self.s.create("b", None),
            lambda: self.s.claim("w", 0, 10),
        ]
        for call in calls:
            with self.assertRaises(RuntimeError):
                call()
        with self.assertRaises(RuntimeError):
            with self.s.transaction():
                pass

    def observe_idempotency_changed_dependencies_conflict(self):
        self.create()
        before = self.snapshot()
        with self.assertRaises(self.m.Conflict):
            self.s.create("a", {"x": [1]}, depends_on=["absent"], idempotency_key="k")
        self.assertEqual(self.snapshot(), before)

    def test_unmatched_join_field_does_not_collide(self):
        left = [{"k": 1, "right.x": 9}]
        right = [{"id": 1, "y": 2}, {"id": 2, "x": 3}]
        self.assertEqual(
            self.q.join(left, right, left_on="k", right_on="id"),
            [{"k": 1, "right.x": 9, "right.id": 1, "right.y": 2}],
        )

    def test_join_and_aggregate_reject_container_keys(self):
        for value in ([], {}):
            with self.assertRaises(self.m.ValidationError):
                self.q.join([{"k": value}], [], left_on="k", right_on="k")
            with self.assertRaises(self.m.ValidationError):
                self.q.aggregate(
                    [{"k": value}], group_by=["k"], metrics={"n": {"op": "count"}}
                )

    def test_invalid_specs_rejected_even_on_empty_input(self):
        for kw in (
            {"where": {"or": [None, {"bad": 1}]}},
            {"order_by": [1]},
            {"fields": [""]},
            {"offset": False},
        ):
            with self.assertRaises(self.m.ValidationError):
                self.q.query([], **kw)
        for spec in ({"op": []}, {"op": "sum"}, {"op": "count", "field": ""}):
            with self.assertRaises(self.m.ValidationError):
                self.q.aggregate([], metrics={"n": spec})

    def test_sort_null_direction_and_stable_ties(self):
        rows = [
            {"v": None, "i": 0},
            {"v": 1, "i": 1},
            {"i": 2},
            {"v": 2, "i": 3},
            {"v": 1, "i": 4},
        ]
        self.assertEqual(
            [r["i"] for r in self.q.query(rows, order_by=["v"])], [1, 4, 3, 0, 2]
        )
        self.assertEqual(
            [r["i"] for r in self.q.query(rows, order_by=["-v"])], [3, 1, 4, 0, 2]
        )
        self.assertEqual(rows[0], {"v": None, "i": 0})

    def test_projection_and_join_results_are_deep_copies(self):
        rows = [{"nested": {"a": [1]}, "k": 1}]
        out = self.q.query(rows, fields=["nested"])
        out[0]["nested"]["a"].append(2)
        self.assertEqual(rows[0]["nested"]["a"], [1])
        right = [{"k": 1, "b": [3]}]
        out = self.q.join(rows, right, left_on="k", right_on="k")
        out[0]["right.b"].append(4)
        self.assertEqual(right[0]["b"], [3])

    def test_staged_export_not_committed_on_rollback(self):
        self.create()
        before = self.s.events()
        path = pathlib.Path(self.tmp.name) / "export.json"
        with self.assertRaises(RuntimeError):
            with self.s.transaction():
                self.s.create("b", None)
                self.s.export(path)
                raise RuntimeError("abort")
        self.assertEqual(
            [j["id"] for j in json.loads(path.read_text())["jobs"]], ["a", "b"]
        )
        self.assertEqual(self.s.events(), before)
        with self.m.Store(self.tmp.name) as other:
            self.assertEqual([j["id"] for j in other.list_jobs()], ["a"])

    def test_expression_validation_reaches_short_circuited_branch(self):
        bad = {"field": "x", "op": "prefix", "value": 5}
        with self.assertRaises(self.m.ValidationError):
            self.q.matches({}, {"or": [None, bad]})
        with self.assertRaises(self.m.ValidationError):
            self.q.matches({}, {"and": [{"or": []}, bad]})


if __name__ == "__main__":
    sys.path.insert(0, str(pathlib.Path(sys.argv[1]).resolve()))
    result = Result()
    unittest.defaultTestLoader.loadTestsFromTestCase(QualityChecks).run(result)
    # Changed key parameters require Conflict, but unknown dependencies require
    # NotFound. The specification did not define precedence when both apply.
    observations = Result()
    QualityChecks("observe_idempotency_changed_dependencies_conflict").run(observations)
    print(
        json.dumps(
            {
                "posthoc": True,
                "passed": sum(r["passed"] for r in result.records),
                "total": result.testsRun,
                "checks": result.records,
                "unscored_ambiguity": observations.records,
                "ambiguity_note": "Conflict versus NotFound precedence was not specified",
            }
        )
    )
