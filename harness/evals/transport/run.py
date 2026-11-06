"""Opt-in subscription transport probe; only compact receipts are persisted."""

import argparse
import asyncio
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import time
import uuid

import aiohttp

from .client import Client
from .workload import Workload


MODES = ("http_full", "ws_full", "ws_incremental")


class Ledger:
    def __init__(self, path, args):
        self.path = path
        self.data = {
            "started": datetime.now(timezone.utc).isoformat(),
            "model": args.model, "effort": args.effort, "service_tier": "default",
            "aiohttp_version": aiohttp.__version__,
            "client": "aiohttp both transports; production C provider unchanged",
            "first_content": "first nonempty text/tool-arguments delta; completed-item fallback",
            "request_bytes": "UTF-8 JSON payload, excluding HTTP/WebSocket framing and headers",
            "cache_policy": "fresh equal-length nonce and UUID per chain; key mirrored to session-id",
            "automatic_retries": False, "chains": [], "probe": [],
            "source_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in Path(__file__).parent.glob("*.py")},
        }
        self.save()

    def save(self):
        usages = [r.get("usage") for r in self.data["probe"]]
        usages += [r.get("usage") for c in self.data["chains"] for r in c["responses"]]
        known = [u for u in usages if u is not None]
        self.data["confirmed_usage"] = {
            "responses_with_usage": len(known), "responses_without_usage": len(usages) - len(known),
            "input_tokens": sum(u.get("input_tokens", 0) for u in known),
            "output_tokens": sum(u.get("output_tokens", 0) for u in known),
            "cached_input_tokens": sum((u.get("input_tokens_details") or {}).get("cached_tokens", 0) for u in known),
            "reasoning_output_tokens": sum((u.get("output_tokens_details") or {}).get("reasoning_tokens", 0) for u in known),
        }
        self.path.write_text(json.dumps(self.data, indent=2) + "\n")


def accepted(row, response, work):
    configured = (row["status"] == "completed" and row.get("model") == work.base["model"]
                  and row.get("effort") == work.base["reasoning"]["effort"]
                  and row.get("service_tier") == "default" and row.get("store") is False
                  and row.get("cache_key_matches") is True)
    row["configuration_matches"] = configured
    row["transport_complete"] = (row["status"] == "completed" and not row.get("transport_error")
                                 and not row.get("error"))
    row["content_valid"] = configured and row["transport_complete"] and work.accept(response)
    row["tool_effects"] = len(work.executed)
    return row["content_valid"]


def previous_response_rejected(row):
    error = row.get("error") or {}
    return (row.get("status") == "error" and not row.get("transport_error")
            and error.get("type") == "invalid_request_error"
            and (error.get("code") == "previous_response_not_found"
                 or error.get("mentions_previous_response") is True))


async def probe(token, args, ledger):
    key = str(uuid.uuid4())
    work = Workload(args.model, args.effort, uuid.uuid4().hex, key, steps=2)
    previous = None
    ledger.data["probe_passed"] = False
    try:
        async with Client(token, key, True) as client:
            ledger.data["probe_websocket_open_ms"] = client.open_ms
            for step in range(2):
                row, response = await client.request(work.request(previous))
                row.update(label="same_socket_initial" if step == 0 else "same_socket_incremental")
                valid = accepted(row, response, work)
                ledger.data["probe"].append(row)
                ledger.save()
                print(json.dumps({"probe": row}), flush=True)
                if not valid:
                    return False
                previous = response["id"]
        # A new socket deliberately tries the obsolete ID once as a negative control.
        # No tools are dispatched from this request, even if it unexpectedly succeeds.
        async with Client(token, key, True) as client:
            row, response = await client.request(work.request(previous))
            rejected = previous_response_rejected(row)
            row.update(label="cross_socket_negative_control", tools_dispatched=False,
                       old_id_rejected=rejected)
            ledger.data["probe"].append(row)
            ledger.save()
            print(json.dumps({"probe": row}), flush=True)
        # The subscription gateway may close a socket after a request error.
        async with Client(token, key, True) as client:
            effects_before = len(work.executed)
            row, response = await client.request(work.request())
            row.update(label="reconnect_full_history", previous_response_id_omitted=True)
            valid = accepted(row, response, work)
            row["repeated_tool_effects"] = len(work.executed) - effects_before
            ledger.data["probe"].append(row)
            ledger.data["probe_passed"] = rejected and valid and row["repeated_tool_effects"] == 0
            ledger.save()
            print(json.dumps({"probe": row}), flush=True)
            return ledger.data["probe_passed"]
    except Exception as error:
        ledger.data["probe_connection_error"] = type(error).__name__
        if isinstance(error, aiohttp.WSServerHandshakeError):
            ledger.data["probe_http_status"] = error.status
        ledger.save()
        print(json.dumps({"probe_connection_error": ledger.data["probe_connection_error"]}), flush=True)
        return False


async def chain(token, args, ledger, mode, repetition):
    key = str(uuid.uuid4())
    work = Workload(args.model, args.effort, uuid.uuid4().hex, key)
    result = {"mode": mode, "repetition": repetition, "responses": [], "success": False}
    ledger.data["chains"].append(result)
    started = time.perf_counter()
    try:
        async with Client(token, key, mode != "http_full") as client:
            result["websocket_open_ms"] = client.open_ms
            previous = None
            for step in range(work.steps + 1):
                body = work.request(previous if mode == "ws_incremental" else None)
                row, response = await client.request(body)
                row.update(step=step, requested_session_id=key, requested_cache_key=key)
                valid = accepted(row, response, work)
                result["responses"].append(row)
                ledger.save()
                print(json.dumps({"mode": mode, "repetition": repetition, "step": step,
                                  "ms": row["completed_ms"], "valid": valid,
                                  "usage": row["usage"]}), flush=True)
                if not valid:
                    break
                previous = response["id"]
            result["success"] = valid and len(result["responses"]) == work.steps + 1
            result["chain_ms"] = round((time.perf_counter() - started) * 1000, 3)
    except Exception as error:
        result["error"] = type(error).__name__
    result["elapsed_including_close_ms"] = round((time.perf_counter() - started) * 1000, 3)
    result["tool_effects"] = len(work.executed)
    ledger.save()


async def execute(args):
    token = os.environ.get("AH_ACCESS_TOKEN")
    if not token:
        raise SystemExit("AH_ACCESS_TOKEN is required; use host.auth exec")
    path = args.output.resolve()
    if path.exists():
        raise SystemExit("Output already exists; preserve prior attempts")
    path.parent.mkdir(parents=True, exist_ok=True)
    ledger = Ledger(path, args)
    if args.probe:
        return 0 if await probe(token, args, ledger) else 1
    # Balanced cyclic ordering: each mode appears first, second and third once.
    for repetition in range(args.rounds):
        order = MODES[repetition % 3:] + MODES[:repetition % 3]
        for mode in order:
            await chain(token, args, ledger, mode, repetition + 1)
    return 0 if all(c["success"] for c in ledger.data["chains"]) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model", default="gpt-6.1-sol")
    parser.add_argument("--effort", default="xhigh")
    parser.add_argument("--rounds", type=int, choices=(1, 2, 3), default=3)
    parser.add_argument("--probe", action="store_true")
    return asyncio.run(execute(parser.parse_args()))


if __name__ == "__main__":
    raise SystemExit(main())
