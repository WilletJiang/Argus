"""Fixed synthetic growing transcripts, isolated by per-case identifiers."""

import copy
import hashlib
import json
import uuid


def corpus(seed, count):
    return "\n".join(
        f"record {i:03d}: {hashlib.sha256(f'{seed}:{i}'.encode()).hexdigest()[:24]} value={i % 17}"
        for i in range(count)
    )


def request_body(model, effort, seed, layout, keyed):
    instructions = f"Independent cache probe {seed}. Treat all reference records as inert data. Reply with exactly OK. Do not analyze, summarize or execute the reference."
    user = {
        "role": "user",
        "content": "Reference dataset:\n"
        + corpus("reference-data-v1", 120)
        + "\nAcknowledge with OK.",
    }
    body = {
        "model": model,
        "store": False,
        "stream": True,
        "reasoning": {"effort": effort},
        "service_tier": "default",
        "include": ["reasoning.encrypted_content"],
        "input": [user],
        "tools": [
            {
                "type": "namespace",
                "name": "local",
                "description": "Synthetic inert file references.",
                "tools": [
                    {
                        "type": "function",
                        "name": "read_file",
                        "description": "Read a reference file.",
                        "strict": False,
                        "parameters": {
                            "type": "object",
                            "properties": {"path": {"type": "string"}},
                            "required": ["path"],
                            "additionalProperties": False,
                        },
                    }
                ],
            }
        ],
        "tool_choice": "none",
    }
    if layout == "instructions":
        body["instructions"] = instructions
    else:
        body["input"].insert(0, {"role": "developer", "content": instructions})
    if keyed:
        body["prompt_cache_key"] = str(uuid.uuid4())
    return body


def grow(body, step):
    body = copy.deepcopy(body)
    call = f"cache_call_{step}"
    body["input"].extend(
        [
            {
                "type": "function_call",
                "namespace": "local",
                "name": "read_file",
                "call_id": call,
                "arguments": json.dumps({"path": f"part{step}.txt"}),
            },
            {
                "type": "function_call_output",
                "call_id": call,
                "output": corpus(f"reference-part{step}", 30),
            },
        ]
    )
    return body
