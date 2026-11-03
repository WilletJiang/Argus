"""Authorized subscription evaluation. Credential is inherited in memory, never logged."""
import argparse
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import time
from cases import CASES


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--driver', type=Path, required=True)
    parser.add_argument('--codex', default=shutil.which('codex'))
    parser.add_argument('--model', default='gpt-6.1-sol')
    parser.add_argument('--timeout', type=int, default=250)
    args = parser.parse_args()
    if not args.codex or not os.environ.get('AH_ACCESS_TOKEN'):
        parser.error('An authorized AH_ACCESS_TOKEN and Codex sandbox executable are required')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    driver = str(args.driver.resolve())
    codex = args.codex
    model = args.model
    results = []
    for case in CASES:
        workspace = output / case.case_name / 'workspace'
        prompt = case.prepare(workspace)
        state = workspace.parent / 'state.db'
        started = time.monotonic()
        process = subprocess.Popen([driver, str(workspace), str(state), model, codex],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, start_new_session=True)
        timed_out = False
        try:
            stdout, stderr = process.communicate(prompt, timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGTERM)
            try:
                stdout, stderr = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                stdout, stderr = process.communicate()
        elapsed = time.monotonic() - started
        secret = os.environ.get('AH_ACCESS_TOKEN', '')
        if secret and (secret in stdout or secret in stderr):
            raise RuntimeError('Refusing to persist a trace containing a credential')
        events = [json.loads(x) for x in stdout.splitlines() if x.strip()]
        (workspace.parent / 'events.json').write_text(json.dumps(events, ensure_ascii=False, indent=2))
        (workspace.parent / 'stderr.txt').write_text(stderr)
        grade = case.validate(workspace)
        usage = []
        for event in events:
            if event.get('event') != 'model.completed':
                continue
            raw = event['data'].get('usage') or {}
            usage.append({
                'input_tokens': raw.get('input_tokens'),
                'output_tokens': raw.get('output_tokens'),
                'cached_input_tokens': raw.get('input_tokens_details', {}).get('cached_tokens'),
                'reasoning_output_tokens': raw.get('output_tokens_details', {}).get('reasoning_tokens'),
            })
        state_json = next((e for e in reversed(events) if 'state' in e and 'model' in e), {})
        result = {'runner':'harness','case':case.case_name,'model':model,'reasoning':'low',
            'seconds':round(elapsed,3),'exit_code':process.returncode,'timed_out':timed_out,
            'model_calls':state_json.get('model_calls'),'tool_calls':state_json.get('tool_calls'),
            'state':state_json.get('state'),'usage':usage,**grade}
        results.append(result)
        (output / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2))
        print(json.dumps(result,ensure_ascii=False),flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
