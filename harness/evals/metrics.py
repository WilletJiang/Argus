"""Comparable usage accounting; cached and reasoning tokens remain subsets."""


def summarize(result):
    totals = {
        "input_tokens": 0,
        "cached_input_tokens": 0,
        "output_tokens": 0,
        "reasoning_output_tokens": 0,
    }
    calls = 0
    tools = 0
    active = set()
    errors = []
    served = set()
    efforts = set()
    last_activity = 0
    for row in result.pop("events"):
        e = row["event"]
        seconds = row["seconds"]
        kind = e.get("event") or e.get("method", "")
        if result["runner"] == "harness":
            if kind in ("model.completed", "model.late_result"):
                usage = e.get("data", {}).get("usage")
                if usage:
                    calls += 1
                    totals["input_tokens"] += usage.get("input_tokens", 0)
                    totals["output_tokens"] += usage.get("output_tokens", 0)
                    totals["cached_input_tokens"] += usage.get(
                        "input_tokens_details", {}
                    ).get("cached_tokens", 0)
                    totals["reasoning_output_tokens"] += usage.get(
                        "output_tokens_details", {}
                    ).get("reasoning_tokens", 0)
                if e.get("data", {}).get("model"):
                    served.add(e["data"]["model"])
                if (e.get("data", {}).get("reasoning") or {}).get("effort"):
                    efforts.add(e["data"]["reasoning"]["effort"])
            if kind == "tool.started":
                tools += 1
                active.add(int(seconds // 60))
                last_activity = seconds
            if kind == "run.stopped":
                errors.append(e.get("data", {}))
        else:
            params = e.get("params", {})
            if kind == "thread/tokenUsage/updated":
                usage = params.get("tokenUsage", {}).get("total", {})
                new = {
                    k: usage.get(field, 0)
                    for k, field in [
                        ("input_tokens", "inputTokens"),
                        ("output_tokens", "outputTokens"),
                        ("cached_input_tokens", "cachedInputTokens"),
                        ("reasoning_output_tokens", "reasoningOutputTokens"),
                    ]
                }
                if new != totals:
                    calls += 1
                    totals = new
            if kind == "item/started" and params.get("item", {}).get("type") in (
                "commandExecution",
                "fileChange",
            ):
                tools += 1
                active.add(int(seconds // 60))
                last_activity = seconds
            if kind in ("error", "model/rerouted"):
                errors.append(params)
    result.update(
        usage=totals,
        confirmed_model_responses=calls,
        observed_tool_actions=tools,
        tool_active_minutes=sorted(active),
        last_tool_started_seconds=round(last_activity, 3),
        served_models=sorted(served),
        served_reasoning_efforts=sorted(efforts),
        errors=errors,
    )
    result["usage_complete"] = (
        not result["timed_out"] and result["state"] == "completed"
    )
    return result
