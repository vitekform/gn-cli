"""The AI tool commands (gn tools, gn tool, gn logs, gn memory, gn web) against a local server.

A disposable HTTP server stands in for GET /api/v2/ai/tools and
POST /api/v2/ai/tools/{name}/run, records what the binary sends, and answers
like the backend. Fake token, temporary config directory, no production services.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', required=True)
args, unittest_args = parser.parse_known_args()
binary = str(Path(args.binary).resolve())

TOOLS = [
    {'name': 'logs_search', 'description': 'Search the central log. More text.',
     'parameters': {'type': 'object', 'properties': {
         'where': {'type': 'string', 'description': 'SQL-like filter'}, 'limit': {'type': 'integer'}}}},
    {'name': 'read_memory_global', 'description': 'Read a global memory.',
     'parameters': {'type': 'object', 'properties': {'id': {'type': 'integer'}}, 'required': ['id']}},
    {'name': 'search_web', 'description': 'Search the web.',
     'parameters': {'type': 'object', 'properties': {
         'query': {'type': 'string'}, 'freshness': {'type': 'string', 'enum': ['day', 'week']}},
         'required': ['query']}},
]


class Server:
    def __init__(self):
        self.calls = []
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def reply(self, code, obj):
                body = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                outer.calls.append(('GET', self.path, self.headers.get('X-CLI-Token'), None))
                if self.path == '/api/v2/ai/tools':
                    return self.reply(200, TOOLS)
                self.reply(404, {'status': 404, 'message': 'no such path'})

            def do_POST(self):
                raw = self.rfile.read(int(self.headers.get('Content-Length') or 0)).decode()
                body = json.loads(raw or '{}')
                outer.calls.append(('POST', self.path, self.headers.get('X-CLI-Token'), body))
                if body.get('where') == 'fail':
                    return self.reply(400, {'status': 400, 'message': "bad query near 'fail'"})
                name = self.path.split('/')[5]
                self.reply(200, {'tool': name, 'result': json.dumps({'ran': name})})

        self.httpd = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.url = 'http://127.0.0.1:%d' % self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class AiToolsTest(unittest.TestCase):
    def setUp(self):
        self.server = Server()
        self.config = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.server.close()
        self.config.cleanup()

    def gn(self, *argv, env_extra=None):
        env = dict(os.environ, GN_CLI_CONFIG_DIR=self.config.name, GN_ACCEPT_TERMS='1')
        for key in ('GN_TOKEN', 'GN_API_URL', 'GN_SANDBOX_CHAT'):
            env.pop(key, None)
        env.update(env_extra or {})
        return subprocess.run([binary, '--api-url', self.server.url, '--token', 'gnc_fake', '--no-color',
                               '--no-unicode', *argv], env=env, input='', capture_output=True, text=True, timeout=10)

    def posts(self):
        return [(path, body) for method, path, _, body in self.server.calls if method == 'POST']

    def test_tools_lists_and_describes(self):
        r = self.gn('tools')
        self.assertEqual(r.returncode, 0, r.stderr)
        for name in ('logs_search', 'read_memory_global', 'search_web'):
            self.assertIn(name, r.stdout)
        self.assertIn('Search the central log.', r.stdout)
        self.assertNotIn('More text', r.stdout)  # the first sentence only

        r = self.gn('tools', 'search_web')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('query string (required)', r.stdout)
        self.assertIn('one of: day, week', r.stdout)

        r = self.gn('--json', 'tools')
        self.assertEqual([t['name'] for t in json.loads(r.stdout)], [t['name'] for t in TOOLS])

        self.assertEqual(self.gn('tools', 'nope').returncode, 1)

    def test_key_values_become_json_arguments(self):
        r = self.gn('tool', 'logs_search', 'where=status >= 500', 'limit=20', 'flag=true', 'ids=[1,2]', 'day=2026-11-05')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(json.loads(r.stdout), {'ran': 'logs_search'})
        self.assertEqual(self.posts(), [('/api/v2/ai/tools/logs_search/run',
                                         {'where': 'status >= 500', 'limit': 20, 'flag': True, 'ids': [1, 2],
                                          'day': '2026-11-05'})])
        self.assertTrue(all(token == 'gnc_fake' for _, _, token, _ in self.server.calls))

    def test_short_forms(self):
        self.assertEqual(self.gn('logs', 'search', 'where=x', '--limit', '5').returncode, 0)
        self.assertEqual(self.gn('memory', 'read', '5', env_extra={'GN_SANDBOX_CHAT': '42'}).returncode, 0)
        self.assertEqual(self.gn('--chat', '43', 'memory', 'read-chat', '#7').returncode, 0)
        self.assertEqual(self.gn('memory', 'list', env_extra={'GN_SANDBOX_CHAT': '42'}).returncode, 0)
        self.assertEqual(self.gn('web', 'fetch', 'https://a.example', 'https://b.example').returncode, 0)
        self.assertEqual(self.gn('web', 'search', 'hello', 'world', '--limit', '3').returncode, 0)
        self.assertEqual(self.gn('web', 'research', 'what', 'is', 'up').returncode, 0)
        self.assertEqual(self.posts(), [
            ('/api/v2/ai/tools/logs_search/run', {'where': 'x', 'limit': 5}),
            ('/api/v2/ai/tools/read_memory_global/run?chatId=42', {'id': 5}),
            ('/api/v2/ai/tools/read_memory_chat/run?chatId=43', {'id': 7}),
            ('/api/v2/ai/tools/list_memories/run?chatId=42', {}),
            ('/api/v2/ai/tools/fetch_web_page/run', {'urls': ['https://a.example', 'https://b.example']}),
            ('/api/v2/ai/tools/search_web/run', {'query': 'hello world', 'count': 3}),
            ('/api/v2/ai/tools/research_web/run', {'query': 'what is up'}),
        ])

    def test_errors_are_reported_without_a_request_where_possible(self):
        r = self.gn('logs', 'search', 'where=fail')
        self.assertEqual(r.returncode, 1)
        self.assertIn("bad query near 'fail'", r.stdout + r.stderr)

        before = len(self.server.calls)
        for argv in (('memory', 'read', 'abc'), ('tool', 'logs_search', 'notkv'), ('tool',),
                     ('memory', 'read', '5', '--chat', 'x')):
            with self.subTest(argv=argv):
                r = self.gn(*argv)
                self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertEqual(len(self.server.calls), before)


if __name__ == '__main__':
    unittest.main(argv=[__file__, *unittest_args])
