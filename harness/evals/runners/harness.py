"""C runtime adapter; shared tracing and credential handling live outside workloads."""

import asyncio
import json
import os
import pathlib
import time
from ..trace import Trace
from .common import terminate, drain_stderr


async def run_harness(
    directory, driver, codex, model, effort, prompt, duration, barrier=None
):
    directory = pathlib.Path(directory)
    workspace = directory / "workspace"
    trace = Trace(directory, os.environ["AH_ACCESS_TOKEN"])
    state = {}
    timed_out = False
    if barrier:
        try:
            await asyncio.wait_for(barrier.wait(), 60)
        except BaseException:
            trace.close()
            raise
    started = time.monotonic()
    trace.start = started
    p = await asyncio.create_subprocess_exec(
        str(driver),
        str(workspace),
        str(directory / "state.db"),
        model,
        codex,
        effort,
        str(duration + 15),
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        start_new_session=True,
        limit=8 * 1024 * 1024,
    )
    p.stdin.write(prompt.encode())
    await p.stdin.drain()
    p.stdin.close()
    stderr = asyncio.create_task(
        drain_stderr(p, directory / "stderr.txt", trace.secret)
    )

    async def read():
        nonlocal state
        while True:
            line = await p.stdout.readline()
            if not line:
                break
            event = json.loads(line)
            trace.write(event)
            if "model" in event and "state" in event:
                state = event

    reader = asyncio.create_task(read())
    try:
        await asyncio.wait_for(asyncio.shield(reader), duration)
    except asyncio.TimeoutError:
        timed_out = True
        await terminate(p)
        await reader
    finally:
        await terminate(p)
        await stderr
        trace.close()
    return {
        "runner": "harness",
        "seconds": round(time.monotonic() - started, 3),
        "timed_out": timed_out,
        "exit_code": p.returncode,
        "state": state.get("state"),
        "model_calls": state.get("model_calls"),
        "tool_calls": state.get("tool_calls"),
        "events": trace.events,
    }
