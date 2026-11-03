"""Independent frozen behavioral checks; run outside the agent workspace."""

import copy, importlib, json, pathlib, random, subprocess, sys, tempfile, unittest


from endurance_store_checks import StoreChecks


class QueryChecks(unittest.TestCase):
    def setUp(self):
        self.q = importlib.import_module("workboard.query")
        self.V = importlib.import_module("workboard").ValidationError

    def test_predicates(self):
        r = {"a": {"x": 3}, "s": "hello", "n": None, "l": [1, 2]}
        for field, op, value, want in [
            ("a.x", "gte", 3, True),
            ("s", "prefix", "he", True),
            ("l", "contains", 2, True),
            ("s", "contains", "ell", True),
            ("missing", "eq", None, True),
            ("n", "exists", True, True),
            ("missing", "exists", False, True),
            ("a.x", "lt", "4", False),
            ("s", "in", ["hello"], True),
            ("missing", "ne", 3, True),
        ]:
            self.assertEqual(
                self.q.matches(r, {"field": field, "op": op, "value": value}), want
            )

    def test_logic_validation(self):
        self.assertTrue(self.q.matches({}, {"and": []}))
        self.assertFalse(self.q.matches({}, {"or": []}))
        self.assertTrue(self.q.matches({}, {"not": {"or": []}}))
        for expr in (
            {"and": [{"or": []}, {"bogus": 1}]},
            {"field": "x", "op": "in", "value": "wrong"},
            {"field": "", "op": "eq", "value": 1},
            {"or": False},
        ):
            with self.assertRaises(self.V):
                self.q.matches({}, expr)

    def test_query_sort_projection(self):
        rows = [
            {"g": 1, "x": None},
            {"g": 2, "x": 1},
            {"g": 1, "x": 3},
            {"g": 1, "x": 3, "z": 9},
        ]
        original = copy.deepcopy(rows)
        out = self.q.query(
            iter(rows), order_by=["g", "-x"], fields=["x", "z"], offset=1, limit=2
        )
        self.assertEqual(out, [{"x": 3, "z": 9}, {"x": None, "z": None}])
        self.assertEqual(rows, original)

    def test_query_filter_copy(self):
        rows = [{"x": 1, "p": {"a": [2]}}, {"x": 2}]
        out = self.q.query(rows, where={"field": "x", "op": "eq", "value": 1})
        out[0]["p"]["a"].append(3)
        self.assertEqual(rows[0]["p"]["a"], [2])
        for kw in (
            {"offset": True},
            {"limit": -1},
            {"order_by": [""]},
            {"fields": [""]},
        ):
            with self.assertRaises(self.V):
                self.q.query(rows, **kw)

    def test_join_many_to_many(self):
        left = [{"k": 1, "v": "a"}, {"k": None, "v": "n"}, {"k": 1, "v": "b"}]
        right = [{"id": 1, "x": 2}, {"id": 1, "x": 3}]
        out = self.q.join(iter(left), iter(right), left_on="k", right_on="id")
        self.assertEqual(
            [(x["v"], x["right.x"]) for x in out],
            [("a", 2), ("a", 3), ("b", 2), ("b", 3)],
        )
        self.assertNotIn("right.x", left[0])

    def test_join_left_and_collision(self):
        out = self.q.join(
            [{"k": None}, {"k": 9}],
            [{"id": None, "x": 1}, {"id": 1, "y": 2}],
            left_on="k",
            right_on="id",
            how="left",
        )
        self.assertEqual(
            out,
            [
                {"k": None, "right.id": None, "right.x": None, "right.y": None},
                {"k": 9, "right.id": None, "right.x": None, "right.y": None},
            ],
        )
        with self.assertRaises(self.V):
            self.q.join(
                [{"k": 1, "right.id": 7}], [{"id": 1}], left_on="k", right_on="id"
            )

    def test_aggregate(self):
        rows = [{"g": "a", "x": 2}, {"g": "b", "x": None}, {"g": "a", "x": 4}]
        metrics = {
            "rows": {"op": "count"},
            "n": {"op": "count", "field": "x"},
            "s": {"op": "sum", "field": "x"},
            "a": {"op": "avg", "field": "x"},
            "lo": {"op": "min", "field": "x"},
        }
        self.assertEqual(
            self.q.aggregate(rows, group_by=["g"], metrics=metrics),
            [
                {"g": "a", "rows": 2, "n": 2, "s": 6, "a": 3, "lo": 2},
                {"g": "b", "rows": 1, "n": 0, "s": 0, "a": None, "lo": None},
            ],
        )

    def test_aggregate_empty_invalid(self):
        self.assertEqual(
            self.q.aggregate([], metrics={"n": {"op": "count"}}), [{"n": 0}]
        )
        self.assertEqual(
            self.q.aggregate([], group_by=["x"], metrics={"n": {"op": "count"}}), []
        )
        for row in ({"x": True}, {"x": "x"}):
            with self.assertRaises(self.V):
                self.q.aggregate([row], metrics={"s": {"op": "sum", "field": "x"}})
        with self.assertRaises(self.V):
            self.q.aggregate([{"g": 1}], group_by=["g"], metrics={"g": {"op": "count"}})

    def test_random_sort(self):
        rng = random.Random(28)
        rows = [
            {"a": rng.randrange(4), "b": rng.randrange(5), "i": i} for i in range(100)
        ]
        self.assertEqual(
            self.q.query(rows, order_by=["a", "-b"]),
            sorted(rows, key=lambda r: (r["a"], -r["b"])),
        )


class PlanningChecks(unittest.TestCase):
    def setUp(self):
        self.p = importlib.import_module("workboard.planning")
        self.m = importlib.import_module("workboard")

    def jobs(self):
        return [
            {"id": "a", "sequence": 1, "depends_on": [], "state": "succeeded"},
            {"id": "b", "sequence": 2, "depends_on": ["a"], "state": "queued"},
            {"id": "c", "sequence": 3, "depends_on": ["a", "b"], "state": "queued"},
        ]

    def test_layers_and_blocked(self):
        jobs = self.jobs()
        original = copy.deepcopy(jobs)
        self.assertEqual(self.p.plan(jobs[::-1]), [["a"], ["b"], ["c"]])
        self.assertEqual(self.p.blocked(jobs), {"c": ["b"]})
        self.assertEqual(jobs, original)

    def test_invalid_graph(self):
        j = self.jobs()
        j[0]["depends_on"] = ["c"]
        with self.assertRaises(self.m.ValidationError):
            self.p.plan(j)
        with self.assertRaises(self.m.ValidationError):
            self.p.plan(self.jobs() + [self.jobs()[0]])
        j = self.jobs()
        j[0]["depends_on"] = ["missing"]
        with self.assertRaises(self.m.NotFound):
            self.p.plan(j)
        with self.assertRaises(self.m.NotFound):
            self.p.blocked(j)
        self.assertEqual(self.p.plan([]), [])


class CliChecks(unittest.TestCase):
    def invoke(self, d, commands):
        p = subprocess.run(
            [sys.executable, "-m", "workboard", "--db", d],
            input="\n".join(
                x if isinstance(x, str) else json.dumps(x) for x in commands
            )
            + "\n",
            capture_output=True,
            text=True,
            timeout=8,
        )
        self.assertEqual(p.returncode, 0, p.stderr[-300:])
        lines = p.stdout.splitlines()
        self.assertEqual(len(lines), len(commands))
        return [json.loads(x) for x in lines]

    def test_cli_roundtrip_errors(self):
        with tempfile.TemporaryDirectory() as d:
            rows = self.invoke(
                d,
                [
                    {"op": "create", "id": "a", "payload": None},
                    "broken json",
                    {"op": "get", "id": "a"},
                    {"op": "get", "id": "missing"},
                    {"op": "get", "id": "a", "extra": 1},
                ],
            )
            self.assertTrue(rows[0]["ok"])
            self.assertTrue(rows[2]["ok"])
            self.assertEqual(
                [rows[i]["error"]["type"] for i in (1, 3, 4)],
                ["ValidationError", "NotFound", "ValidationError"],
            )
            self.assertEqual(
                self.invoke(d, [{"op": "list"}])[0]["result"][0]["id"], "a"
            )

    def test_cli_batch_rollback(self):
        with tempfile.TemporaryDirectory() as d:
            rows = self.invoke(
                d,
                [
                    {
                        "op": "batch",
                        "commands": [
                            {"op": "create", "id": "a", "payload": 1},
                            {"op": "create", "id": "a", "payload": 2},
                        ],
                    },
                    {"op": "list"},
                    {
                        "op": "batch",
                        "commands": [
                            {"op": "create", "id": "b", "payload": 1},
                            {"op": "cancel", "id": "b"},
                        ],
                    },
                    {"op": "plan"},
                ],
            )
            self.assertFalse(rows[0]["ok"])
            self.assertEqual(rows[1]["result"], [])
            self.assertTrue(rows[2]["ok"])
            self.assertEqual(rows[3]["result"], [["b"]])


class Result(unittest.TestResult):
    def __init__(self):
        super().__init__()
        self.records = []

    def addSuccess(self, test):
        super().addSuccess(test)
        self.records.append({"test": test.id().split(".")[-1], "passed": True})

    def addFailure(self, test, err):
        super().addFailure(test, err)
        self.records.append(
            {
                "test": test.id().split(".")[-1],
                "passed": False,
                "error": str(err[1])[:350],
            }
        )

    def addError(self, test, err):
        super().addError(test, err)
        self.records.append(
            {
                "test": test.id().split(".")[-1],
                "passed": False,
                "error": type(err[1]).__name__ + ": " + str(err[1])[:350],
            }
        )


def main():
    sys.path.insert(0, str(pathlib.Path(sys.argv[1]).resolve()))
    suite = unittest.TestSuite(
        unittest.defaultTestLoader.loadTestsFromTestCase(c)
        for c in (StoreChecks, QueryChecks, PlanningChecks, CliChecks)
    )
    result = Result()
    suite.run(result)
    print(
        json.dumps(
            {
                "passed": sum(x["passed"] for x in result.records),
                "total": result.testsRun,
                "checks": result.records,
            },
            ensure_ascii=False,
        )
    )


if __name__ == "__main__":
    main()
