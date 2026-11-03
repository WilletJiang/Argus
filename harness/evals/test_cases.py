"""Offline checks for the frozen live-evaluation graders, using synthetic files."""

import csv
from decimal import Decimal, ROUND_HALF_UP
import json
from pathlib import Path
import tempfile
import unittest

from cases import CASES


def write_json(root, name, value):
    (root / name).write_text(json.dumps(value, indent=4), encoding="utf-8")


def reconcile(root):
    def rows(name):
        return list(csv.DictReader((root / name).read_text().splitlines()))

    def latest(name, key):
        ordered = sorted(rows(name), key=lambda row: int(row["revision"]))
        return {row[key]: row for row in ordered}.values()

    customers = {row["customer_id"]: row["name"] for row in rows("customers.csv")}
    rates = json.loads((root / "fx.json").read_text())
    payments = list(latest("payments.csv", "payment_id"))
    invoices = []
    for row in sorted(latest("invoices.csv", "invoice_id"), key=lambda row: row["invoice_id"]):
        if row["state"] != "active" or row["issued_on"] > "2026-11-01":
            continue
        paid = sum((Decimal(p["amount"]) for p in payments
                    if p["invoice_id"] == row["invoice_id"] and p["state"] == "posted"
                    and p["posted_on"] <= "2026-11-01"), Decimal(0))
        rate = Decimal(rates[row["currency"]])

        def cents(amount):
            return int((amount * rate * 100).quantize(Decimal(1), rounding=ROUND_HALF_UP))

        total, paid = cents(Decimal(row["amount"])), cents(paid)
        balance = total - paid
        status = "overpaid" if balance < 0 else "paid" if balance == 0 else "unpaid" if paid == 0 else "partial"
        invoices.append(dict(invoice_id=row["invoice_id"], customer=customers[row["customer_id"]],
                             total_usd_cents=total, paid_usd_cents=paid,
                             balance_usd_cents=balance, status=status))
    totals = {key: sum(row[key] for row in invoices)
              for key in ("total_usd_cents", "paid_usd_cents", "balance_usd_cents")}
    return dict(as_of="2026-11-01", invoices=invoices,
                totals=dict(invoice_count=len(invoices), **totals))


CORRECT_INTERVALS = '''def coalesce(intervals):
    try:
        source = iter(intervals)
    except TypeError as exc:
        raise ValueError("Expected iterable") from exc
    ordered = []
    for pair in source:
        if not isinstance(pair, (list, tuple)) or len(pair) != 2:
            raise ValueError("Expected pair")
        start, end = pair
        if any(type(value) is not int for value in pair) or start > end:
            raise ValueError("Invalid endpoints")
        if start != end:
            ordered.append((start, end))
    result = []
    for start, end in sorted(ordered):
        if result and start <= result[-1][1]:
            result[-1] = (result[-1][0], max(result[-1][1], end))
        else:
            result.append((start, end))
    return result
'''


def migrate(root):
    config = json.loads((root / "config.json").read_text())
    config.update(endpoint="/v3/messages", timeout_ms=1200)
    config["headers"]["X-Protocol-Version"] = "3"
    write_json(root, "config.json", config)
    lines = {'API_PATH = "/v2/messages"\n': 'API_PATH = "/v3/messages"\n',
             "TIMEOUT_MS = 800\n": "TIMEOUT_MS = 1200\n",
             "Active endpoint: `/v2/messages`.\n": "Active endpoint: `/v3/messages`.\n",
             "Request timeout: 800 ms.\n": "Request timeout: 1200 ms.\n"}
    for name in ("client.py", "protocol.md"):
        content = (root / name).read_text().splitlines(keepends=True)
        (root / name).write_text("".join(lines.get(line, line) for line in content))
    write_json(root, "release.json", dict(protocol_version=3,
                                        changed_files=["client.py", "config.json", "protocol.md"]))


class CaseTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ah-eval-grader-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def prepare(self, name):
        case = next(case for case in CASES if case.case_name == name)
        root = self.root / name
        case.prepare(root)
        return case, root

    def assert_grade(self, case, root, expected):
        result = case.validate(root)
        self.assertIs(result["pass"], expected, result["details"])
        self.assertTrue(result["details"])

    def test_prepare_is_deterministic_and_refuses_overwrite(self):
        for case in CASES:
            with self.subTest(case=case.case_name):
                first, second = self.root / case.case_name, self.root / (case.case_name + "-copy")
                self.assertEqual(case.prepare(first), case.prepare(second))
                self.assertEqual({p.name: p.read_bytes() for p in first.iterdir()},
                                 {p.name: p.read_bytes() for p in second.iterdir()})
                self.assert_grade(case, first, False)
                with self.assertRaises(ValueError):
                    case.prepare(first)

    def test_reconciliation_correct_then_rejects_types_schema_and_rounding(self):
        case, root = self.prepare("reconcile")
        correct = reconcile(root)
        self.assertEqual(correct["totals"], dict(invoice_count=5, total_usd_cents=3868,
                                               paid_usd_cents=1946, balance_usd_cents=1922))
        write_json(root, "result.json", correct)
        self.assert_grade(case, root, True)
        for bad in (False, 0.0, 1):
            with self.subTest(balance=repr(bad)):
                output = reconcile(root)
                output["invoices"][0]["balance_usd_cents"] = bad
                write_json(root, "result.json", output)
                self.assert_grade(case, root, False)
        for output in ([], {**correct, "approved": True}, {"invoices": correct["invoices"]}):
            write_json(root, "result.json", output)
            self.assert_grade(case, root, False)
        correct["invoices"][3]["paid_usd_cents"] = 12  # Per-payment rounding is wrong.
        write_json(root, "result.json", correct)
        self.assert_grade(case, root, False)
        (root / "result.json").write_text("not JSON")
        self.assert_grade(case, root, False)

    def test_reconciliation_requires_preserved_inputs(self):
        case, root = self.prepare("reconcile")
        write_json(root, "result.json", reconcile(root))
        (root / "customers.csv").write_text("overwritten")
        self.assert_grade(case, root, False)

    def test_interval_solution_and_boundary_mutations(self):
        case, root = self.prepare("intervals")
        (root / "intervals.py").write_text(CORRECT_INTERVALS)
        self.assert_grade(case, root, True)
        mutations = [("start <= result[-1][1]", "start < result[-1][1]"),
                     ("max(result[-1][1], end)", "end"),
                     ("type(value) is not int", "not isinstance(value, int)"),
                     ("if start != end:", "if True:")]
        for before, after in mutations:
            with self.subTest(mutation=before):
                (root / "intervals.py").write_text(CORRECT_INTERVALS.replace(before, after))
                self.assert_grade(case, root, False)
        (root / "intervals.py").write_text(CORRECT_INTERVALS)
        (root / "usage.txt").write_text("changed")
        self.assert_grade(case, root, False)

    def test_migration_requires_exact_scope_and_schema(self):
        case, root = self.prepare("update")
        migrate(root)
        self.assert_grade(case, root, True)
        snapshot = {p.name: p.read_bytes() for p in root.iterdir()}
        variants = [("sentinel.txt", "changed"), ("legacy_client.py", "changed"),
                    ("client.py", snapshot["client.py"].decode() + "# extra\n"),
                    ("protocol.md", snapshot["protocol.md"].decode().replace("/v2/", "/v3/")),
                    ("extra.txt", "unexpected"), ("release.json", '{"protocol_version":3}'),
                    ("config.json", snapshot["config.json"].decode().replace('"retries": 2', '"retries": false')),
                    ("release.json", snapshot["release.json"].decode().replace('"protocol_version": 3', '"protocol_version": 3.0'))]
        for name, content in variants:
            with self.subTest(file=name):
                (root / name).write_text(content)
                self.assert_grade(case, root, False)
                if name in snapshot:
                    (root / name).write_bytes(snapshot[name])
                else:
                    (root / name).unlink()


if __name__ == "__main__":
    unittest.main()
