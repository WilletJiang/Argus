"""Public executable contracts against a deterministic Responses service, no secrets/network API."""
import http.server
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading

CLI = sys.argv[1]
requests = []


class Service(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        requests.append(body)
        assert body['store'] is False and body['stream'] is True
        assert body['tools'][0]['type'] == 'namespace'
        items = body['input']
        outputs = [x for x in items if x.get('type') == 'function_call_output']
        model = body['model']
        if not outputs and model in ('read', 'write', 'ask'):
            name, args = {
                'read': ('read_file', {'path': 'source.txt'}),
                'write': ('write_file', {'path': 'created.txt', 'content': 'persisted result'}),
                'ask': ('harness_ask', {'question': 'Which output?', 'options': ['short', 'long']}),
            }[model]
            output = [{'type': 'function_call', 'id': 'fc_1', 'call_id': 'call_1',
                       'namespace': 'local', 'name': name, 'arguments': json.dumps(args), 'status': 'completed'}]
        else:
            output = [{'type': 'message', 'id': 'msg_1', 'role': 'assistant', 'status': 'completed',
                       'content': [{'type': 'output_text', 'text': 'Verified result', 'annotations': []}]}]
        response = {'id': 'resp_1', 'status': 'completed', 'output': output,
                    'usage': {'input_tokens': 10, 'output_tokens': 5, 'total_tokens': 15}}
        event = {'type': 'response.completed', 'response': response}
        data = ('data: ' + json.dumps(event) + '\n\n').encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Service)
threading.Thread(target=server.serve_forever, daemon=True).start()
endpoint = f'http://127.0.0.1:{server.server_port}/responses'
env = dict(os.environ, AH_ACCESS_TOKEN='local-fixture-not-a-credential')

with tempfile.TemporaryDirectory(prefix='ah-cli-') as tmp:
    workspace = pathlib.Path(tmp) / 'workspace'
    workspace.mkdir()
    (workspace / 'source.txt').write_text('hello 世界')
    base = ['--db', tmp + '/state.db', '--workspace', str(workspace),
            '--endpoint', endpoint, '--allow-http-loopback']

    def run(command, *args, ok=True):
        result = subprocess.run([CLI, command, *base, *args], env=env,
                                capture_output=True, text=True, timeout=10)
        if ok:
            assert result.returncode == 0, (command, result.stdout, result.stderr)
        else:
            assert result.returncode != 0, (command, result.stdout)
        assert 'local-fixture-not-a-credential' not in result.stdout + result.stderr
        lines = [json.loads(x) for x in result.stdout.splitlines() if x.strip()]
        return lines[-1] if lines else None

    plain = run('new', '--model', 'simple')['id']
    state = run('run', '--session', plain, '--prompt', 'Hello', '--command-id', 'u1')
    assert state['state'] == 'completed'
    before = len(requests)
    run('run', '--session', plain, '--prompt', 'Hello', '--command-id', 'u1')
    assert len(requests) == before, 'Duplicate command must not execute again'
    run('run', '--session', plain, '--prompt', 'Changed', '--command-id', 'u1', ok=False)
    read = run('new', '--model', 'read')['id']
    assert run('run', '--session', read, '--prompt', 'Read source', '--command-id', 'r1')['state'] == 'completed'
    assert 'hello' in json.dumps(requests[-1]['input'])
    write = run('new', '--model', 'write')['id']
    state = run('run', '--session', write, '--prompt', 'Write file', '--command-id', 'w1')
    assert state['state'] == 'waiting_approval' and not (workspace / 'created.txt').exists()
    operation = state['pending']['operation_id']
    run('approve', '--session', write, '--operation', operation)
    assert run('run', '--session', write)['state'] == 'completed'
    assert (workspace / 'created.txt').read_text() == 'persisted result'
    ask = run('new', '--model', 'ask')['id']
    state = run('run', '--session', ask, '--prompt', 'Ask me', '--command-id', 'a1')
    assert state['state'] == 'waiting_input'
    operation = state['pending']['operation_id']
    run('reply', '--session', ask, '--operation', operation, '--value', '"short"')
    assert run('run', '--session', ask)['state'] == 'completed'
    forked = run('fork', '--session', plain)['id']
    assert forked != plain
    run('compact', '--session', plain, '--summary', 'A prior greeting completed.', '--keep-turns', '0')
    assert run('inspect', '--session', plain)['state'] == 'completed'
    run('events', '--session', plain)
    assert len(run('list')) >= 5
    # RPC framing, IDs, unknown methods, and complete requests without a model call.
    script = '\n'.join(json.dumps(x) for x in [
        {'id': 'c', 'method': 'create', 'model': 'simple'},
        {'id': 2, 'method': 'list'}, {'id': 3, 'method': 'unknown'}]) + '\n'
    p = subprocess.run([CLI, 'rpc', *base], input=script, capture_output=True, text=True, env=env, timeout=10)
    assert p.returncode == 0, p.stderr
    replies = {x['id']: x for x in map(json.loads, p.stdout.splitlines()) if 'id' in x}
    assert replies['c']['status'] == 'ok' and replies[2]['status'] == 'ok'
    assert replies[3]['status'] == 'invalid'

server.shutdown()
print('CLI integration passed: real HTTP tool loop, approval/restart, Ask/reply, dedupe, fork, compaction, RPC')
