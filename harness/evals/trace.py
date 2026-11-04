"""Content-free request fingerprints and cache accounting for Responses requests."""

import hashlib
import json
import pathlib
import time

FIELDS = (
    "model",
    "instructions",
    "tools",
    "reasoning",
    "text",
    "service_tier",
    "tool_choice",
    "parallel_tool_calls",
    "include",
    "store",
    "stream",
    "prompt_cache_key",
)


def encoded(value):
    return json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    ).encode()


def digest(value):
    return hashlib.sha256(encoded(value)).hexdigest()


def number(value):
    return (
        value
        if isinstance(value, int) and not isinstance(value, bool) and value >= 0
        else None
    )


def counts(value):
    if not isinstance(value, dict):
        return {}
    return {
        key: number(value.get(key))
        for key in (
            "input_tokens",
            "cached_tokens",
            "cache_write_tokens",
            "output_tokens",
        )
        if key in value
    }


class CacheTrace:
    def __init__(self):
        self.previous = None
        self.requests = 0

    def request(self, body):
        if not isinstance(body, dict) or not isinstance(body.get("input"), list):
            raise ValueError("A Responses request with array input is required")
        items = []
        for item in body["input"]:
            items.append(
                {
                    "sha256": digest(item),
                    "bytes": len(encoded(item)),
                    "type": item.get("type"),
                    "role": item.get("role"),
                    "id_sha256": digest(item["id"]) if item.get("id") else None,
                }
            )
        fields = {key: digest([key in body, body.get(key)]) for key in FIELDS}
        options = body.get("prompt_cache_options") or {}
        fields["cache_policy"] = digest(
            {k: v for k, v in options.items() if k != "comparison_response_id"}
        )
        common = 0
        old = self.previous
        if old:
            for before, after in zip(old["items"], items):
                if before["sha256"] != after["sha256"]:
                    break
                common += 1
        rewritten = bool(old and common < len(old["items"]))
        size = sum(item["bytes"] for item in items)
        matched_bytes = sum(item["bytes"] for item in items[:common])
        self.requests += 1
        result = {
            "kind": "cache.request",
            "request_index": self.requests,
            "request_sha256": digest(body),
            "request_bytes": len(encoded(body)),
            "model": body.get("model"),
            "fields": fields,
            "items": items,
            "input_items": len(items),
            "input_bytes": size,
            "previous_input_items": len(old["items"]) if old else 0,
            "matching_prior_items": common,
            "matching_prior_bytes": matched_bytes,
            "rewritten_prior_input": rewritten,
            "appended_input_bytes": None if rewritten else size - matched_bytes,
            "changed_fields": [
                k for k in fields if old and fields[k] != old["fields"][k]
            ],
            "cache_key_present": bool(body.get("prompt_cache_key")),
            "comparison_requested": bool(options.get("comparison_response_id")),
            "comparison_response_id": options.get("comparison_response_id"),
            "cache_key_sha256": digest(body["prompt_cache_key"])
            if body.get("prompt_cache_key")
            else None,
        }
        self.previous = {
            "fields": fields,
            "items": items,
            "cache_key_sha256": result["cache_key_sha256"],
        }
        return result

    def response(self, body, *, elapsed_ms=None, transport_error=None):
        body = body if isinstance(body, dict) else {}
        usage = body.get("usage") or {}
        inputs = usage.get("input_tokens_details") or {}
        outputs = usage.get("output_tokens_details") or {}
        diagnostics = body.get("prompt_cache_diagnostics")
        result = {
            "kind": "cache.response",
            "request_index": self.requests,
            "response_id": body.get("id"),
            "model": body.get("model"),
            "service_tier": body.get("service_tier"),
            "cache_key_echo_sha256": digest(body["prompt_cache_key"])
            if body.get("prompt_cache_key")
            else None,
            "cache_policy_echo": {
                k: v
                for k, v in (body.get("prompt_cache_options") or {}).items()
                if k in ("mode", "ttl")
            },
            "legacy_retention_echo": body.get("prompt_cache_retention"),
            "status": body.get("status"),
            "reasoning_effort": (body.get("reasoning") or {}).get("effort"),
            "elapsed_ms": elapsed_ms,
            "transport_error": transport_error,
            "usage_present": bool(usage),
            "input_tokens": number(usage.get("input_tokens")),
            "cached_input_tokens": number(inputs.get("cached_tokens")),
            "cache_write_tokens": number(inputs.get("cache_write_tokens")),
            "output_tokens": number(usage.get("output_tokens")),
            "reasoning_output_tokens": number(outputs.get("reasoning_tokens")),
            "total_tokens": number(usage.get("total_tokens")),
            "diagnostics_returned": isinstance(diagnostics, dict),
            "diagnostics": {
                k: diagnostics.get(k)
                for k in (
                    "type",
                    "reason",
                    "comparison_reusable_tokens",
                    "cache_missed_tokens",
                )
                if k in diagnostics
            }
            if isinstance(diagnostics, dict)
            else None,
        }
        expected = (self.previous or {}).get("cache_key_sha256")
        echoed = result["cache_key_echo_sha256"]
        result["cache_key_echo_matches_request"] = (
            expected == echoed if expected is not None and echoed is not None else None
        )
        if (
            result["input_tokens"] is not None
            and result["cached_input_tokens"] is not None
        ):
            result["uncached_input_tokens"] = (
                result["input_tokens"] - result["cached_input_tokens"]
            )
        else:
            result["uncached_input_tokens"] = None
        attribution = usage.get("attribution") or {}
        result["attribution"] = {
            "request_fields": {
                name: counts(value)
                for name, value in (attribution.get("request_fields") or {}).items()
            },
            "items": [
                {"id_sha256": digest(name), **counts(value)}
                for name, value in (attribution.get("items") or {}).items()
            ],
        }
        return result


class Trace:
    def __init__(self, directory, secret):
        self.directory = pathlib.Path(directory)
        self.secret = secret
        self.start = time.monotonic()
        self.cache = CacheTrace()
        self.cache_file = None
        self.events = []
        self.count = 0
        self.last_progress = 0
        self.file = (self.directory / "trace.jsonl").open("w")

    def write(self, event):
        if event.get("event") in ("provider.request", "provider.response"):
            if self.cache_file is None:
                self.cache_file = (self.directory / "cache-metrics.jsonl").open("w")
            if event["event"] == "provider.request":
                safe = self.cache.request(event["data"])
            else:
                safe = self.cache.response(
                    event.get("data"),
                    elapsed_ms=event.get("elapsed_ms"),
                    transport_error=event.get("transport_error"),
                )
            self.cache_file.write(json.dumps(safe) + "\n")
            self.cache_file.flush()
            event = {"event": safe["kind"], "data": safe}
        raw = json.dumps(event, ensure_ascii=False)
        if self.secret and self.secret in raw:
            raise RuntimeError("Refusing trace containing credential")
        row = {"seconds": round(time.monotonic() - self.start, 3), "event": event}
        self.file.write(json.dumps(row, ensure_ascii=False) + "\n")
        self.file.flush()
        compact = dict(event)
        if event.get("event") in ("model.completed", "model.late_result"):
            compact["data"] = {
                k: event.get("data", {}).get(k) for k in ("usage", "model", "reasoning")
            }
        self.events.append({"seconds": row["seconds"], "event": compact})
        self.count += 1
        if time.monotonic() - self.last_progress >= 1:
            self.last_progress = time.monotonic()
            (self.directory / "progress.json").write_text(
                json.dumps(
                    {
                        "events": self.count,
                        "last_event_seconds": row["seconds"],
                        "last_event": event.get("event") or event.get("method"),
                    }
                )
            )

    def close(self):
        self.file.close()
        if self.cache_file:
            self.cache_file.close()
