"""Run bounded smoke tasks through the shared, subscription-authorized adapters."""

import argparse
import asyncio
import json
import os
from pathlib import Path
import shutil
from .cases import CASES
from ..metrics import summarize
from ..runners.codex import run_codex
from ..runners.harness import run_harness


async def run(args):
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    if any(root.iterdir()):
        raise ValueError("A fresh empty output directory is required")
    runners = ("harness", "codex") if args.runner == "both" else (args.runner,)
    results = []
    for case in CASES:
        if args.case and case.case_name != args.case:
            continue
        for runner in runners:
            directory = root / f"{case.case_name}-{runner}"
            workspace = directory / "workspace"
            prompt = case.prepare(workspace)
            if runner == "harness":
                result = await run_harness(
                    directory,
                    args.driver.resolve(),
                    args.codex,
                    args.model,
                    args.effort,
                    prompt,
                    args.timeout,
                )
            else:
                result = await run_codex(
                    directory, args.codex, args.model, args.effort, prompt, args.timeout
                )
            result = summarize(result)
            result.update(
                case=case.case_name,
                model=args.model,
                reasoning=args.effort,
                grade=case.validate(workspace),
            )
            results.append(result)
            (root / "results.json").write_text(
                json.dumps(results, ensure_ascii=False, indent=2)
            )
            print(json.dumps(result, ensure_ascii=False), flush=True)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--runner", choices=("harness", "codex", "both"), default="both"
    )
    parser.add_argument("--driver", type=Path)
    parser.add_argument("--codex", default=shutil.which("codex"))
    parser.add_argument("--model", default="gpt-6.1-sol")
    parser.add_argument(
        "--effort", choices=("low", "medium", "high", "xhigh"), default="low"
    )
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("--case", choices=[case.case_name for case in CASES])
    args = parser.parse_args()
    if not args.codex or not os.environ.get("AH_ACCESS_TOKEN"):
        parser.error("An authorized AH_ACCESS_TOKEN and Codex executable are required")
    if args.runner != "codex" and not args.driver:
        parser.error("--driver is required for the harness")
    if not 1 <= args.timeout <= 1800:
        parser.error("--timeout must be 1..1800 seconds")
    results = asyncio.run(run(args))
    return (
        0
        if all(r["grade"]["pass"] and r["state"] == "completed" for r in results)
        else 1
    )


if __name__ == "__main__":
    raise SystemExit(main())
