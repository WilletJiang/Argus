"""Codex app-server adapter with per-response usage and external interruption."""

import asyncio, json, os, pathlib, time
from endurance_runner import COMMON, Trace, terminate, drain_stderr


class AppServer:
    def __init__(self, process, trace):
        self.p = process
        self.trace = trace
        self.serial = 0
        self.pending = {}
        self.finished = asyncio.Event()
        self.turn_id = None
        self.thread_id = None
        self.status = None
        self.model = None

    async def send(self, value):
        self.p.stdin.write((json.dumps(value) + "\n").encode())
        await self.p.stdin.drain()

    async def request(self, method, params, timeout=45):
        self.serial += 1
        id = self.serial
        future = asyncio.get_running_loop().create_future()
        self.pending[id] = future
        await self.send({"id": id, "method": method, "params": params})
        answer = await asyncio.wait_for(future, timeout)
        if "error" in answer:
            raise RuntimeError(method + ": " + json.dumps(answer["error"]))
        return answer.get("result")

    async def read(self):
        try:
            while True:
                line = await self.p.stdout.readline()
                if not line:
                    break
                message = json.loads(line)
                self.trace.write(message)
                if "id" in message and ("result" in message or "error" in message):
                    future = self.pending.pop(message["id"], None)
                    if future and not future.done():
                        future.set_result(message)
                elif "id" in message:
                    await self.send(
                        {
                            "id": message["id"],
                            "error": {
                                "code": -32000,
                                "message": "Interactive host input is unavailable; follow the supplied specification.",
                            },
                        }
                    )
                else:
                    params = message.get("params", {})
                    if message.get("method") == "turn/started":
                        self.turn_id = params.get("turn", {}).get("id")
                    if message.get("method") == "turn/completed":
                        self.status = params.get("turn", {}).get("status")
                        self.finished.set()
        finally:
            for future in self.pending.values():
                if not future.done():
                    future.set_exception(RuntimeError("app-server closed"))
            self.finished.set()


async def run_codex(
    directory,
    codex,
    model,
    effort,
    prompt,
    duration,
    barrier=None,
    base_url="https://api.openai.com/v1",
    access_token=None,
):
    directory = pathlib.Path(directory)
    workspace = directory / "workspace"
    home = directory / "codex-home"
    home.mkdir()
    token = os.environ["AH_ACCESS_TOKEN"] if access_token is None else access_token
    trace = Trace(directory, token)
    env = dict(
        os.environ, CODEX_HOME=str(home), ACCESS_TOKEN=os.environ["AH_ACCESS_TOKEN"]
    )
    env.pop("OPENAI_API_KEY", None)
    env.pop("AH_ACCESS_TOKEN", None)
    configuration = {
        "model_provider": "evaluation_plan",
        "model_providers.evaluation_plan.name": "ChatGPT plan",
        "model_providers.evaluation_plan.base_url": base_url,
        "model_providers.evaluation_plan.env_key": "ACCESS_TOKEN",
        "model_providers.evaluation_plan.wire_api": "responses",
        "model_providers.evaluation_plan.requires_openai_auth": False,
        "model_providers.evaluation_plan.supports_websockets": False,
        "model_providers.evaluation_plan.request_max_retries": 0,
        "model_providers.evaluation_plan.stream_max_retries": 0,
        "model_providers.evaluation_plan.stream_idle_timeout_ms": 900000,
        "project_doc_max_bytes": 0,
        "web_search": "disabled",
        "model_reasoning_effort": effort,
        "shell_environment_policy.inherit": "core",
        "shell_environment_policy.exclude": [
            "ACCESS_TOKEN",
            "AH_ACCESS_TOKEN",
            "OPENAI_API_KEY",
        ],
        "features.apps": False,
        "features.plugins": False,
        "features.memories": False,
        "features.multi_agent": False,
        "features.skill_search": False,
        "features.skip_host_skill_discovery": True,
        "features.image_generation": False,
        "features.browser_use": False,
        "features.computer_use": False,
        "features.goals": False,
    }
    argv = [codex, "app-server", "--listen", "stdio://"]
    for key, value in configuration.items():
        argv.extend(["-c", key + "=" + json.dumps(value)])
    p = await asyncio.create_subprocess_exec(
        *argv,
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        env=env,
        start_new_session=True,
        limit=8 * 1024 * 1024,
    )
    app = AppServer(p, trace)
    reader = asyncio.create_task(app.read())
    stderr = asyncio.create_task(
        drain_stderr(p, directory / "stderr.txt", trace.secret)
    )
    started = None
    timed_out = False
    failure = None
    try:
        await app.request(
            "initialize",
            {
                "clientInfo": {
                    "name": "argus_harness_evaluation",
                    "title": "Argus Harness Evaluation",
                    "version": "0.2.0",
                },
                "capabilities": {"experimentalApi": True},
            },
        )
        await app.send({"method": "initialized", "params": {}})
        info = await app.request(
            "thread/start",
            {
                "model": model,
                "modelProvider": "evaluation_plan",
                "cwd": str(workspace),
                "approvalPolicy": "never",
                "sandbox": "workspace-write",
                "ephemeral": True,
                "developerInstructions": COMMON,
            },
        )
        app.thread_id = info["thread"]["id"]
        app.model = info.get("model")
        if barrier:
            await asyncio.wait_for(barrier.wait(), 60)
        started = time.monotonic()
        trace.start = started
        turn = await app.request(
            "turn/start",
            {
                "threadId": app.thread_id,
                "model": model,
                "effort": effort,
                "input": [{"type": "text", "text": prompt}],
                "serviceTierForTurn": "default",
            },
        )
        app.turn_id = turn["turn"]["id"]
        remaining = max(0.01, duration - (time.monotonic() - started))
        try:
            await asyncio.wait_for(app.finished.wait(), remaining)
        except asyncio.TimeoutError:
            timed_out = True
            await app.request(
                "turn/interrupt",
                {"threadId": app.thread_id, "turnId": app.turn_id},
                timeout=5,
            )
            try:
                await asyncio.wait_for(app.finished.wait(), 5)
            except asyncio.TimeoutError:
                pass
    except Exception as e:
        failure = str(e)
        if barrier and started is None:
            await barrier.abort()
    finally:
        await terminate(p)
        await reader
        await stderr
        trace.close()
    return {
        "runner": "codex",
        "seconds": round(time.monotonic() - started, 3) if started else 0,
        "timed_out": timed_out,
        "exit_code": p.returncode,
        "state": app.status,
        "reported_model": app.model,
        "failure": failure,
        "events": trace.events,
    }
