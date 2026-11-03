"""Three paired, concurrent long-task trials with an undisclosed external cutoff."""

import argparse, asyncio, hashlib, json, os, pathlib, shutil, subprocess
from endurance_task import prepare, protected_hashes, PROMPT, SPEC
from endurance_runner import StartBarrier, run_harness, summarize
from endurance_codex import run_codex
from endurance_wire import verify as verify_wire


async def grade(directory, protected, check=None):
    workspace = directory / "workspace"
    check = check or pathlib.Path(__file__).with_name("endurance_checks.py")
    env = {
        "PATH": "/usr/bin:/bin",
        "HOME": str(directory),
        "PYTHONDONTWRITEBYTECODE": "1",
    }
    p = await asyncio.create_subprocess_exec(
        "/usr/bin/python3",
        str(check),
        str(workspace),
        cwd=workspace,
        env=env,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        start_new_session=True,
    )
    try:
        stdout, stderr = await asyncio.wait_for(p.communicate(), 45)
        result = json.loads(stdout.decode().splitlines()[-1])
    except (asyncio.TimeoutError, json.JSONDecodeError, IndexError) as e:
        if p.returncode is None:
            p.kill()
        await p.wait()
        result = {"passed": 0, "total": 33, "grader_error": type(e).__name__}
    try:
        result["protected_inputs_unchanged"] = protected_hashes(workspace) == protected
    except OSError:
        result["protected_inputs_unchanged"] = False
    result["readme_written"] = (workspace / "README.md").exists() and len(
        (workspace / "README.md").read_text()
    ) > 300
    result["additional_test_files"] = len(
        [
            p
            for p in (workspace / "tests").rglob("test_*.py")
            if p.name != "test_smoke.py"
        ]
    )
    return result


async def run(args):
    root = args.output.resolve()
    if root.exists() and any(root.iterdir()):
        raise RuntimeError("A fresh empty output directory is required")
    root.mkdir(parents=True, exist_ok=True)
    if not os.environ.get("AH_ACCESS_TOKEN"):
        raise RuntimeError("Authorized subscription token required")
    wire = await verify_wire(args.codex, args.model, args.effort)
    criteria = root / "criteria"
    criteria.mkdir()
    for name in ("endurance_checks.py", "endurance_store_checks.py"):
        shutil.copy2(pathlib.Path(__file__).with_name(name), criteria / name)
    manifest = {
        "model": args.model,
        "reasoning": args.effort,
        "duration_seconds": args.duration,
        "repetitions": args.repetitions,
        "same_service_route": True,
        "codex_wire_request": wire,
        "endpoint": "https://api.openai.com/v1/responses",
        "prompt_sha256": hashlib.sha256(PROMPT.encode()).hexdigest(),
        "spec_sha256": hashlib.sha256(SPEC.encode()).hexdigest(),
        "grader_sha256": hashlib.sha256(
            b"".join(
                (criteria / name).read_bytes()
                for name in ("endurance_checks.py", "endurance_store_checks.py")
            )
        ).hexdigest(),
        "cutoff_disclosed_to_model": False,
        "followup_prompts": 0,
        "codex_version": subprocess.check_output(
            [args.codex, "--version"], text=True
        ).strip(),
    }
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2))
    # Smoke runs are explicitly separate from task/token totals.
    preflights = []
    for runner in ("harness", "codex"):
        d = root / ("preflight-" + runner)
        d.mkdir()
        (d / "workspace").mkdir()
        (d / "workspace" / "README.md").write_text(
            "Empty model access probe workspace.\n"
        )
        prompt = "Reply with exactly READY. Do not call tools."
        if runner == "harness":
            r = await run_harness(
                d, args.driver, args.codex, args.model, args.effort, prompt, 120
            )
        else:
            r = await run_codex(d, args.codex, args.model, args.effort, prompt, 120)
        r = summarize(r)
        preflights.append(r)
        print(json.dumps({"preflight": r}), flush=True)
        (root / "preflights.json").write_text(json.dumps(preflights, indent=2))
        if runner == "harness" and (
            r["served_models"] != [args.model]
            or r["served_reasoning_efforts"] != [args.effort]
        ):
            raise RuntimeError(
                "Harness response did not confirm requested model and effort"
            )
        if r["state"] != "completed" or r.get("failure") or r["errors"]:
            raise RuntimeError(
                runner + " preflight did not verify requested model/route"
            )
    barrier = StartBarrier(args.repetitions * 2)
    tasks = []
    results = []

    async def one(runner, index):
        d = root / (runner + "-" + str(index))
        prompt = prepare(d / "workspace")
        hashes = protected_hashes(d / "workspace")
        print(json.dumps({"launching": runner, "trial": index}), flush=True)
        if runner == "harness":
            r = await run_harness(
                d,
                args.driver,
                args.codex,
                args.model,
                args.effort,
                prompt,
                args.duration,
                barrier,
            )
        else:
            r = await run_codex(
                d, args.codex, args.model, args.effort, prompt, args.duration, barrier
            )
        r = summarize(r)
        r["trial"] = index
        r["model"] = args.model
        r["reasoning"] = args.effort
        r["grade"] = await grade(d, hashes, criteria / "endurance_checks.py")
        (d / "result.json").write_text(json.dumps(r, ensure_ascii=False, indent=2))
        results.append(r)
        (root / "results.json").write_text(
            json.dumps(results, ensure_ascii=False, indent=2)
        )
        print(json.dumps({"finished": r}, ensure_ascii=False), flush=True)
        return r

    for i in range(1, args.repetitions + 1):
        for name in ("harness", "codex"):
            tasks.append(asyncio.create_task(one(name, i)))
    while any(not task.done() for task in tasks):
        await asyncio.sleep(10)
        summary = []
        for d in sorted(root.glob("*-[123]")):
            path = d / "progress.json"
            if path.exists():
                try:
                    last = json.loads(path.read_text())
                except json.JSONDecodeError:
                    continue
                summary.append(
                    {
                        "run": d.name,
                        **last,
                        "result_ready": (d / "result.json").exists(),
                    }
                )
        print(json.dumps({"progress": summary}), flush=True)
    await asyncio.gather(*tasks)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--output", type=pathlib.Path, required=True)
    p.add_argument("--driver", type=pathlib.Path, required=True)
    p.add_argument("--codex", default=shutil.which("codex"))
    p.add_argument("--model", default="gpt-6.1-terra")
    p.add_argument(
        "--effort", default="xhigh", choices=["low", "medium", "high", "xhigh"]
    )
    p.add_argument("--duration", type=int, default=600)
    p.add_argument("--repetitions", type=int, default=3)
    args = p.parse_args()
    if not 1 <= args.duration <= 1800 or not 1 <= args.repetitions <= 3:
        p.error("duration 1..1800; repetitions 1..3")
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
