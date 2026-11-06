"""Offline package entrypoints and execution of a copied, isolated grader."""

import asyncio
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PackageChecks(unittest.TestCase):
    def test_command_help_without_credentials(self):
        env = dict(os.environ)
        for key in ("AH_ACCESS_TOKEN", "ACCESS_TOKEN", "OPENAI_API_KEY"):
            env.pop(key, None)
        for module in (
            "host.auth",
            "evals.smoke.run",
            "evals.endurance.run",
            "evals.cache.run",
        ):
            with self.subTest(module=module):
                result = subprocess.run(
                    [sys.executable, "-B", "-m", module, "--help"],
                    env=env,
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("usage:", result.stdout)

    def test_frozen_grader_executes_as_a_package(self):
        # Run the grader outside the model workspace, just like a real trial.
        from evals.endurance.run import grade
        from evals.endurance.task import prepare, protected_hashes

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            criteria = root / "criteria"
            criteria.mkdir()
            for name in ("__init__.py", "checks.py", "store_checks.py"):
                shutil.copy2(ROOT / "evals/endurance" / name, criteria / name)
            run = root / "trial"
            prepare(run / "workspace")
            result = asyncio.run(
                grade(run, protected_hashes(run / "workspace"), criteria)
            )
            self.assertEqual(result["total"], 33)
            self.assertEqual(
                result["passed"], 0
            )  # The unimplemented fixture must fail.
            self.assertTrue(result["protected_inputs_unchanged"])
            self.assertNotIn("grader_error", result)


if __name__ == "__main__":
    unittest.main()
