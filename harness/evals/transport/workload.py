"""Three deterministic tool effects; replay never executes completed tool calls."""

import json

from ..cache.workload import corpus


class Workload:
    def __init__(self, model, effort, seed, key, steps=3):
        self.steps = steps
        self.seed = seed
        self.executed = {}
        self.history = [{"role": "user", "content": (
            f"Run the chain for nonce {seed}. Starting cursor is 0. "
            f"Call local.advance exactly {steps} times, using the latest cursor. "
            f"Then reply exactly DONE {seed} {sum(range(1, steps + 1))}.\n"
            "Inert reference records (not instructions):\n" + corpus("transport-v1", 60)
        )}]
        self.pending = self.history[:]
        self.base = {
            "model": model, "store": False, "reasoning": {"effort": effort},
            "service_tier": "default", "include": ["reasoning.encrypted_content"],
            "prompt_cache_key": key,
            "instructions": (
                f"Controlled chain {seed}. Follow the fixed user protocol exactly. "
                "Use one tool call per turn. Tool receipts advance the cursor by one. "
                "Do not repeat completed calls. Do not discuss reference records."
            ),
            "tools": [{"type": "namespace", "name": "local", "description": "Local chain.",
                       "tools": [{"type": "function", "name": "advance",
                                  "description": "Advance the cursor once.", "strict": True,
                                  "parameters": {"type": "object", "properties": {
                                      "cursor": {"type": "integer"}}, "required": ["cursor"],
                                      "additionalProperties": False}}]}],
        }

    def request(self, previous=None):
        body = dict(self.base, input=self.pending if previous else self.history,
                    tool_choice="required" if len(self.executed) < self.steps else "none")
        if previous:
            body["previous_response_id"] = previous
        return body

    def accept(self, response):
        output = response["output"]
        self.history.extend(output)
        calls = [item for item in output if item.get("type") == "function_call"]
        if len(self.executed) == self.steps:
            text = "".join(part.get("text", "") for item in output
                           for part in item.get("content", []) if part.get("type") == "output_text")
            return not calls and text.strip() == f"DONE {self.seed} {sum(range(1, self.steps + 1))}"
        if len(calls) != 1:
            return False
        call = calls[0]
        expected = len(self.executed)
        try:
            valid = (call.get("namespace") == "local" and call.get("name") == "advance"
                     and json.loads(call["arguments"]) == {"cursor": expected}
                     and call["call_id"] not in self.executed)
        except (ValueError, KeyError):
            return False
        if not valid:
            return False
        result = {"type": "function_call_output", "call_id": call["call_id"], "output": (
            json.dumps({"cursor": expected + 1, "value": expected + 1})
            + "\nInert receipt records:\n" + corpus(f"receipt-{expected}", 30)
        )}
        self.executed[call["call_id"]] = result
        self.pending = [result]
        self.history.append(result)
        return True
