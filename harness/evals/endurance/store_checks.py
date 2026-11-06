"""Independent frozen behavioral checks; run outside the agent workspace."""

import importlib, json, math, pathlib, random, tempfile, unittest


class StoreChecks(unittest.TestCase):
    def setUp(self):
        self.m = importlib.import_module("workboard")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.s = self.m.Store(self.tmp.name)
        self.addCleanup(self.s.close)

    def create(self, id="a", **kw):
        return self.s.create(id, {"x": [1]}, **kw)

    def running(self, **kw):
        self.create(**kw)
        return self.s.claim("worker", 0, 10)

    def test_record_schema(self):
        j = self.create()
        self.assertEqual(
            set(j),
            set(
                "id payload depends_on priority max_attempts available_at state attempts lease_token lease_until worker result error sequence".split()
            ),
        )
        self.assertEqual((j["state"], j["sequence"], j["attempts"]), ("queued", 1, 0))
        for k in ("lease_token", "lease_until", "worker", "result", "error"):
            self.assertIsNone(j[k])

    def test_copy_isolation(self):
        p = {"a": [1]}
        j = self.s.create("a", p)
        p["a"].append(2)
        j["payload"]["a"].append(3)
        self.assertEqual(self.s.get("a")["payload"], {"a": [1]})
        e = self.s.events()
        e[0]["record"]["payload"]["a"].append(4)
        self.assertEqual(self.s.events()[0]["record"]["payload"], {"a": [1]})

    def test_bad_create_atomic(self):
        for kw in (
            {"priority": True},
            {"max_attempts": 0},
            {"max_attempts": False},
            {"available_at": float("nan")},
            {"available_at": True},
        ):
            with self.assertRaises(self.m.ValidationError):
                self.create(**kw)
        for p in ({1: "x"}, [float("inf")], ("x",), object()):
            with self.assertRaises(self.m.ValidationError):
                self.s.create("a", p, idempotency_key="key")
        self.assertEqual(self.s.events(), [])
        self.assertEqual(self.create()["sequence"], 1)

    def test_id_validation(self):
        for id in ("", "a b", "a\nb", 3, None):
            with self.assertRaises(self.m.ValidationError):
                self.s.create(id, {})

    def test_idempotency(self):
        self.create("root")
        j = self.create(depends_on=["root", "root"], idempotency_key="k")
        self.assertEqual(j["depends_on"], ["root"])
        n = len(self.s.events())
        self.assertEqual(self.create(depends_on=["root"], idempotency_key="k"), j)
        self.assertEqual(len(self.s.events()), n)
        with self.assertRaises(self.m.Conflict):
            self.create(priority=9, idempotency_key="k")
        with self.assertRaises(self.m.Conflict):
            self.create("other", idempotency_key="k")

    def test_dependencies(self):
        with self.assertRaises(self.m.NotFound):
            self.create(depends_on=["missing"])
        self.create("a")
        self.create("b", depends_on=["a"])
        self.create("c", depends_on=["b"])
        with self.assertRaises(self.m.ValidationError):
            self.s.set_dependencies("a", ["c"])
        self.assertEqual(self.s.get("a")["depends_on"], [])
        with self.assertRaises(self.m.NotFound):
            self.s.set_dependencies("a", ["missing"])

    def test_priority_and_availability(self):
        self.create("low", priority=-1)
        self.create("first", priority=3)
        self.create("second", priority=3)
        self.create("later", priority=99, available_at=9)
        self.assertEqual(
            [self.s.claim("w", 0, 10)["id"] for _ in range(3)],
            ["first", "second", "low"],
        )
        self.assertIsNone(self.s.claim("w", 0, 10))
        self.assertEqual(self.s.claim("w", 9, 10)["id"], "later")

    def test_dependency_gate(self):
        self.create("a")
        self.create("b", depends_on=["a"], priority=100)
        a = self.s.claim("w", 0, 10)
        self.assertEqual(a["id"], "a")
        self.assertIsNone(self.s.claim("w", 1, 10))
        self.s.complete("a", a["lease_token"], {}, 2)
        self.assertEqual(self.s.claim("w", 2, 10)["id"], "b")

    def test_lease_ownership(self):
        j = self.running()
        for token, now in [
            ("wrong", 1),
            (j["lease_token"], 10),
            (j["lease_token"], 11),
        ]:
            with self.assertRaises(self.m.Conflict):
                self.s.complete("a", token, {}, now)
        self.assertEqual(self.s.get("a")["state"], "running")
        self.s.complete("a", j["lease_token"], [1], 9)
        self.assertEqual(self.s.get("a")["result"], [1])

    def test_heartbeat(self):
        j = self.running()
        token = j["lease_token"]
        self.assertEqual(self.s.heartbeat("a", token, 1, 2)["lease_until"], 10)
        self.assertEqual(self.s.heartbeat("a", token, 5, 20)["lease_until"], 25)
        with self.assertRaises(self.m.Conflict):
            self.s.heartbeat("a", token, 25, 5)

    def test_reap_boundary_and_budget(self):
        first = self.running(max_attempts=2)
        self.assertIsNone(self.s.claim("w2", 9, 10))
        second = self.s.claim("w2", 10, 10)
        self.assertEqual(second["attempts"], 2)
        self.assertNotEqual(first["lease_token"], second["lease_token"])
        self.assertIsNone(self.s.claim("w3", 20, 10))
        j = self.s.get("a")
        self.assertEqual((j["state"], j["error"]), ("failed", "lease expired"))
        self.assertEqual(
            [e["kind"] for e in self.s.events()],
            ["created", "claimed", "lease_expired", "claimed", "lease_expired"],
        )

    def test_invalid_claim_does_not_reap(self):
        self.running()
        for worker, now, lease in [
            ("", 10, 5),
            ("w", True, 5),
            ("w", 10, 0),
            ("w", math.inf, 5),
        ]:
            with self.assertRaises(self.m.ValidationError):
                self.s.claim(worker, now, lease)
        self.assertEqual(self.s.get("a")["state"], "running")
        self.assertEqual(len(self.s.events()), 2)

    def test_fail_retry(self):
        j = self.running(max_attempts=2)
        j = self.s.fail("a", j["lease_token"], "network", 1, 3)
        self.assertEqual(
            (j["state"], j["available_at"], j["error"]), ("queued", 4, "network")
        )
        self.assertIsNone(self.s.claim("w", 3, 10))
        j = self.s.claim("w", 4, 10)
        self.assertIsNone(j["error"])
        j = self.s.fail("a", j["lease_token"], "last", 5)
        self.assertEqual(j["state"], "failed")
        j = self.s.retry("a", available_at=8)
        self.assertEqual((j["state"], j["attempts"], j["sequence"]), ("queued", 0, 1))

    def test_cancel(self):
        j = self.running()
        self.s.cancel("a")
        n = len(self.s.events())
        self.s.cancel("a")
        self.assertEqual(len(self.s.events()), n)
        with self.assertRaises(self.m.Conflict):
            self.s.complete("a", j["lease_token"], {}, 1)
        self.s.retry("a")
        self.assertEqual(self.s.get("a")["state"], "queued")

    def test_transaction_rollback(self):
        self.create("old")
        old = self.s.events()
        with self.assertRaises(RuntimeError):
            with self.s.transaction():
                self.create("a", idempotency_key="k")
                self.s.cancel("old")
                raise RuntimeError("abort")
        self.assertEqual(self.s.events(), old)
        self.assertEqual(self.s.get("old")["state"], "queued")
        self.assertEqual(self.create("b", idempotency_key="k")["sequence"], 2)

    def test_transaction_visibility(self):
        self.create("old")
        with self.s.transaction():
            self.create("new")
            self.assertEqual(len(self.s.list_jobs()), 2)
            with self.m.Store(self.tmp.name) as other:
                self.assertEqual(len(other.list_jobs()), 1)
            with self.assertRaises(self.m.Conflict):
                with self.s.transaction():
                    pass
            with self.assertRaises(self.m.Conflict):
                self.s.close()
            with self.assertRaises(self.m.Conflict):
                self.s.compact()
        with self.m.Store(self.tmp.name) as other:
            self.assertEqual(len(other.list_jobs()), 2)

    def test_restart_and_compact(self):
        self.create("a", idempotency_key="k")
        j = self.s.claim("w", 0, 10)
        before = self.s.events()
        self.s.compact()
        self.s.close()
        self.s = self.m.Store(self.tmp.name)
        self.addCleanup(self.s.close)
        self.assertEqual(self.s.events(), before)
        self.assertEqual(self.s.get("a")["lease_token"], j["lease_token"])
        self.s.complete("a", j["lease_token"], {"done": True}, 1)
        self.assertEqual(self.create(idempotency_key="k")["state"], "succeeded")
        with self.assertRaises(self.m.Conflict):
            self.s.cancel("a")

    def test_events_and_export(self):
        self.create("a")
        self.create("b")
        self.s.cancel("b")
        self.assertEqual([e["seq"] for e in self.s.events(after=1)], [2, 3])
        for after in (-1, True, 1.5):
            with self.assertRaises(self.m.ValidationError):
                self.s.events(after=after)
        path = pathlib.Path(self.tmp.name) / "out.json"
        self.s.export(path)
        raw = path.read_bytes()
        obj = {"format": 1, "jobs": self.s.list_jobs(), "events": self.s.events()}
        self.assertEqual(
            raw,
            (
                json.dumps(
                    obj, sort_keys=True, ensure_ascii=False, separators=(",", ":")
                )
                + "\n"
            ).encode(),
        )
        self.s.export(path)
        self.assertEqual(path.read_bytes(), raw)

    def test_unknown_and_list(self):
        with self.assertRaises(self.m.NotFound):
            self.s.get("absent")
        self.create("a")
        self.create("b")
        self.s.cancel("b")
        self.assertEqual([j["id"] for j in self.s.list_jobs("cancelled")], ["b"])
        with self.assertRaises(self.m.ValidationError):
            self.s.list_jobs("imaginary")

    def test_random_priority_order(self):
        rng = random.Random(9182)
        jobs = []
        for i in range(80):
            p = rng.randrange(-5, 6)
            self.create(str(i), priority=p)
            jobs.append((p, i))
        expected = [str(i) for p, i in sorted(jobs, key=lambda x: (-x[0], x[1]))]
        self.assertEqual([self.s.claim("w", 0, 1)["id"] for _ in jobs], expected)
