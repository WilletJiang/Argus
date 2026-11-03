"""Controlled cache-key/layout trials using the C Responses transport, not task agents."""

import argparse
import copy
import json
import os
from pathlib import Path
import random
import subprocess
import time
import uuid
from concurrent.futures import ThreadPoolExecutor
from cache_trace import CacheTrace
from cache_workload import request_body, grow


class Client:
    def __init__(self, driver, directory):
        env = dict(os.environ)
        env.pop("OPENAI_API_KEY", None)
        env.pop("AH_EVAL_ENDPOINT", None)
        env.pop("AH_EVAL_ALLOW_LOOPBACK", None)
        self.errors = (directory / "transport-errors.txt").open("w")
        self.process = subprocess.Popen(
            [str(driver)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.errors,
            text=True,
            bufsize=1,
            env=env,
        )

    def request(self, body):
        self.process.stdin.write(
            json.dumps(body, ensure_ascii=False, separators=(",", ":")) + "\n"
        )
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError("C transport exited before returning a receipt")
        return json.loads(line)

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.errors.close()


def send(client, trace, body, log, label):
    request = trace.request(body)
    request["label"] = label
    log.write(json.dumps(request) + "\n")
    log.flush()
    answer = client.request(body)
    response = trace.response(
        answer.get("response"),
        elapsed_ms=answer.get("elapsed_ms"),
        transport_error=None
        if answer.get("ok")
        else {"code": answer.get("code"), "message": answer.get("error")},
    )
    response["label"] = label
    log.write(json.dumps(response) + "\n")
    log.flush()
    return response


def capabilities(args, root):
    directory = root / "capabilities"
    directory.mkdir()
    client = Client(args.driver, directory)
    trace = CacheTrace()
    rows = []
    try:
        body = request_body(
            args.model, args.effort, uuid.uuid4().hex, "instructions", True
        )
        with (directory / "metrics.jsonl").open("w") as log:
            first = send(client, trace, body, log, "key_acceptance")
            rows.append(first)
            if (
                first["status"] != "completed"
                or first["model"] != args.model
                or first["reasoning_effort"] != args.effort
            ):
                raise RuntimeError(
                    "Requested model/effort/cache key did not pass the access probe"
                )
            body["prompt_cache_options"] = {
                "comparison_response_id": first["response_id"]
            }
            second = send(client, trace, body, log, "diagnostic_acceptance")
            rows.append(second)
            supported = second["status"] == "completed"
            if supported:
                baseline = second["response_id"]
                for field in ("instructions", "tools"):
                    changed = copy.deepcopy(body)
                    changed["prompt_cache_options"] = {
                        "comparison_response_id": baseline
                    }
                    if field == "instructions":
                        changed["instructions"] += " Control variant."
                    else:
                        changed["tools"][0]["tools"][0]["description"] += (
                            " Control variant."
                        )
                    rows.append(
                        send(client, trace, changed, log, f"positive_control_{field}")
                    )
    finally:
        client.close()
    result = {
        "key_parameter_accepted": True,
        "diagnostic_parameter_accepted": supported,
        "diagnostic_results_returned": any(r["diagnostics_returned"] for r in rows),
        "responses": rows,
    }
    (root / "capabilities.json").write_text(json.dumps(result, indent=2))
    print(json.dumps({"capabilities": result}), flush=True)
    return supported


def run_case(args, root, name, layout, keyed, diagnostics):
    directory = root / name
    directory.mkdir()
    # Fresh equal-length identifiers isolate trials; all other synthetic data
    # generation and step sizes are identical. The identifiers are not user data.
    seed = uuid.uuid4().hex
    body = request_body(args.model, args.effort, seed, layout, keyed)
    client = Client(args.driver, directory)
    trace = CacheTrace()
    rows = []
    started = time.monotonic()
    try:
        with (directory / "metrics.jsonl").open("w") as log:
            for step in range(4):
                if step >= 2:
                    body = grow(body, step)
                if diagnostics and rows:
                    body["prompt_cache_options"] = {
                        "comparison_response_id": rows[-1]["response_id"]
                    }
                row = send(
                    client,
                    trace,
                    body,
                    log,
                    "cold" if step == 0 else "identical" if step == 1 else "growing",
                )
                rows.append(row)
                print(
                    json.dumps(
                        {
                            "case": name,
                            "step": step,
                            "input": row["input_tokens"],
                            "cached": row["cached_input_tokens"],
                            "diagnostics": row["diagnostics"],
                            "error": row["transport_error"],
                        }
                    ),
                    flush=True,
                )
                if row["status"] != "completed":
                    break
                if row["model"] != args.model or row["reasoning_effort"] != args.effort:
                    row["configuration_mismatch"] = True
                    break
    finally:
        client.close()
    result = {
        "case": name,
        "layout": layout,
        "keyed": keyed,
        "seconds": round(time.monotonic() - started, 3),
        "responses": rows,
    }
    (directory / "result.json").write_text(json.dumps(result, indent=2))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model", default="gpt-6.1-terra")
    parser.add_argument("--effort", default="xhigh")
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--parallel", action="store_true")
    args = parser.parse_args()
    if not os.environ.get("AH_ACCESS_TOKEN"):
        parser.error("A plan-authorized AH_ACCESS_TOKEN is required")
    if not 1 <= args.rounds <= 3:
        parser.error("rounds must be 1..3")
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    (root / "manifest.json").write_text(
        json.dumps(
            {
                "model": args.model,
                "effort": args.effort,
                "rounds": args.rounds,
                "parallel": args.parallel,
                "cache_isolation": "fresh equal-length group identifiers; identical reference records and increments",
                "automatic_retries": False,
            },
            indent=2,
        )
    )
    diagnostics = capabilities(args, root)
    results = []
    for repetition in range(args.rounds):
        variants = [
            (layout, keyed)
            for layout in ("instructions", "developer")
            for keyed in (False, True)
        ]
        random.Random(501 + repetition).shuffle(variants)
        for layout, keyed in variants:
            name = f"r{repetition + 1}-{layout}-{'key' if keyed else 'omit'}"
            results.append(run_case(args, root, name, layout, keyed, diagnostics))
            (root / "results.json").write_text(json.dumps(results, indent=2))
    if args.parallel:
        for keyed in (False, True):
            with ThreadPoolExecutor(max_workers=3) as pool:
                futures = [
                    pool.submit(
                        run_case,
                        args,
                        root,
                        f"parallel-{'key' if keyed else 'omit'}-{i + 1}",
                        "instructions",
                        keyed,
                        diagnostics,
                    )
                    for i in range(3)
                ]
                results.extend(f.result() for f in futures)
            (root / "results.json").write_text(json.dumps(results, indent=2))
    print(
        json.dumps({"completed_cases": len(results), "output": str(root)}), flush=True
    )


if __name__ == "__main__":
    main()
