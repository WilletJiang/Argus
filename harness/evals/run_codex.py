"""Run fixed synthetic cases with the installed, authenticated Codex CLI."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import signal
import time
from cases import CASES


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--model', default='gpt-6.1-sol')
    parser.add_argument('--timeout', type=int, default=240)
    parser.add_argument('--case', choices=[case.case_name for case in CASES])
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    results = []
    for case in CASES:
        if args.case and case.case_name != args.case:
            continue
        workspace = args.output / case.case_name / 'workspace'
        prompt = case.prepare(workspace)
        command = ['codex', 'exec', '--ignore-user-config', '--ephemeral', '--skip-git-repo-check',
                   '--sandbox', 'workspace-write', '-m', args.model,
                   '-c', 'model_reasoning_effort="low"', '-c', 'project_doc_max_bytes=0',
                   '-c', 'approval_policy="never"', '-C', str(workspace), '--json', '-']
        env = dict(os.environ)
        env.pop('AH_ACCESS_TOKEN', None)
        started = time.monotonic()
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, env=env, start_new_session=True)
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
                stdout, stderr = process.communicate(timeout=5)
        elapsed = time.monotonic() - started
        events = []
        for line in stdout.splitlines():
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError:
                pass
        (workspace.parent / 'events.json').write_text(json.dumps(events, ensure_ascii=False, indent=2))
        (workspace.parent / 'stderr.txt').write_text(stderr)
        grade = case.validate(workspace)
        usage = [e.get('usage') for e in events if e.get('type') == 'turn.completed']
        result = {'runner': 'codex', 'case': case.case_name, 'model': args.model,
                  'reasoning': 'low', 'seconds': round(elapsed, 3), 'exit_code': process.returncode,
                  'timed_out': timed_out, 'runner_completed': bool(usage) and process.returncode == 0,
                  'usage': usage, **grade}
        results.append(result)
        (args.output / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2))
        print(json.dumps(result, ensure_ascii=False), flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
