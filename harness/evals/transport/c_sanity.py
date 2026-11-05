"""Two full-history calls through the existing persistent C cache driver."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import uuid

from .client import encode
from .workload import Workload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Output exists; preserve previous attempts")
    env = dict(os.environ)
    for key in ("OPENAI_API_KEY", "AH_EVAL_ENDPOINT", "AH_EVAL_ALLOW_LOOPBACK"):
        env.pop(key, None)
    work = Workload("gpt-6.1-sol", "xhigh", uuid.uuid4().hex, str(uuid.uuid4()))
    result = {"client": "existing C cache_driver, same process for both requests",
              "driver_sha256": hashlib.sha256(args.driver.read_bytes()).hexdigest(),
              "connection_reuse": "persistent provider; driver does not expose libcurl connection counts",
              "responses": []}
    with subprocess.Popen([str(args.driver)], env=env, stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                          text=True, bufsize=1) as process:
        for step in range(2):
            body = dict(work.request(), stream=True)
            wire = encode(body).decode() + "\n"
            started = time.perf_counter()
            process.stdin.write(wire)
            process.stdin.flush()
            receipt = json.loads(process.stdout.readline())
            response = receipt.get("response") or {}
            valid = (receipt.get("ok") and response.get("status") == "completed"
                     and response.get("model") == "gpt-6.1-sol"
                     and (response.get("reasoning") or {}).get("effort") == "xhigh"
                     and work.accept(response))
            row = {"step": step, "request_bytes": len(wire.encode()),
                   "elapsed_ms": receipt.get("elapsed_ms"),
                   "wall_ms": round((time.perf_counter() - started) * 1000, 3),
                   "status": response.get("status"), "valid": bool(valid),
                   "model": response.get("model"), "reasoning": response.get("reasoning"),
                   "service_tier": response.get("service_tier"), "store": response.get("store"),
                   "cache_key_matches": response.get("prompt_cache_key") == body["prompt_cache_key"],
                   "usage": response.get("usage"), "code": receipt.get("code")}
            result["responses"].append(row)
            args.output.write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps({k: row[k] for k in ("step", "elapsed_ms", "valid")}), flush=True)
            if not valid:
                break
        process.stdin.close()
        process.wait(timeout=5)
    return 0 if len(result["responses"]) == 2 and all(r["valid"] for r in result["responses"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
