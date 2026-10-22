#!/usr/bin/env python3
"""Exercise a CLI binary against an isolated local RSYS server. No real credentials or releases.

Run: python3 tests/test_rsys.py --binary /path/to/gn-cli
The production executable is used in CI; rsys_driver.cpp also supports a focused Linux build.
"""
import argparse
import copy
from email.parser import BytesParser
from email.policy import default as email_policy
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import unittest
from urllib.parse import parse_qs, unquote, urlsplit


def field(key, choices):
    return dict(key=key, label=key, required=True, options=[dict(key=v, label=v, deprecated=v == 'retired') for v in choices])


SCHEMAS = {1: [field('platform', ['paper', 'spigot']), field('minecraft_version', ['1.21.1', '1.21.2', 'retired'])],
           2: [], 3: [field('runtime', ['java21'])]}


class State:
    def __init__(self):
        self.projects = {name: dict(slug=name, name=name, isPublic=name != 'private', template=dict(revisionId=1, fields=SCHEMAS[1]))
                         for name in ['plugin', 'private']}
        self.projects['general'] = dict(slug='general', isPublic=True, template=dict(revisionId=2, fields=[]))
        self.projects['legacy'] = dict(slug='legacy', isPublic=True)
        self.releases = {}
        self.files = {}
        self.requests = []
        self.next_id = 100
        self.fail_create = ''
        self.concurrent = False
        self.base = ''

    def add_release(self, project, version, revision=1, status='DRAFT', prerelease=False):
        release = dict(project=project, version=version, status=status, prerelease=prerelease, files=[],
                       title=None, changelog=None, publishedAt='2026-10-21T12:00:00Z')
        self.releases[(project, version)] = dict(release=release, revision=revision, token=0, variants=[])
        return self.releases[(project, version)]

    def add_file(self, record, name, content=b'build', primary=False):
        self.next_id += 1
        file = dict(id=self.next_id, filename=name, size=len(content), sha256=hashlib.sha256(content).hexdigest(),
                    status='READY', primary=primary, partSize=8*1024*1024, partCount=1, contentType='application/octet-stream')
        self.files[file['id']] = dict(file=file, record=record, content=content)
        record['release']['files'].append(file)
        return file

    def add_variant(self, record, selection, files, default=None, key='build', automatic=False):
        variant = dict(key=key, selection=selection, fileIds=[f['id'] for f in files], defaultFileId=(default or files[0])['id'], automatic=automatic)
        record['variants'].append(variant)
        return variant

    def metadata(self, record):
        return dict(version=record['token'], templateRevisionId=record['revision'], fields=SCHEMAS[record['revision']],
                    variants=[dict(key=v['key'], selection=v['selection'], defaultFileId=v['defaultFileId'], automatic=v['automatic'],
                                   files=[self.files[i]['file'] for i in v['fileIds']]) for v in record['variants']])

    def refresh(self, record):
        for v in record['variants']:
            if v['automatic']:
                ready = [f for f in record['release']['files'] if f['status'] == 'READY']
                v['fileIds'] = [f['id'] for f in ready]
                v['defaultFileId'] = next((f['id'] for f in ready if f['primary']), ready[0]['id'] if ready else None)
                record['token'] += 1

    def public_file(self, record, file, private=False):
        release = record['release']
        url = ('https://rsys.ganamaga.me/api/v2/rsys/files/' + str(file['id']) + '/download') if private else (
            self.base + '/' + release['project'] + '/public/v/' + release['version'] + '/' + file['filename'])
        return dict(filename=file['filename'], size=file['size'], sha256=file['sha256'], contentType=file['contentType'], url=url)

    def matches(self, project, query):
        selection = {key[2:]: value[0] for key, value in query.items() if key.startswith('f.')}
        found = []
        for (name, _), r in reversed(list(self.releases.items())):
            if name != project or r['release']['status'] != 'PUBLISHED': continue
            if r['release']['prerelease'] and query.get('prerelease') != ['true']: continue
            for v in r['variants']:
                if v['selection'] != selection or v['defaultFileId'] not in self.files: continue
                result = {k: v for k, v in r['release'].items() if k not in ['status', 'files']}
                result.update(templateRevisionId=r['revision'], selection=v['selection'], variant=v['key'],
                              primary=self.public_file(r, self.files[v['defaultFileId']]['file'], not self.projects[project]['isPublic']),
                              files=[self.public_file(r, self.files[i]['file'], not self.projects[project]['isPublic']) for i in v['fileIds']])
                found.append(result)
        return found


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args): pass

    def handle_request(self):
        s = self.server.state
        url = urlsplit(self.path)
        path, query = unquote(url.path), parse_qs(url.query)
        raw = self.rfile.read(int(self.headers.get('content-length', 0)))
        body = json.loads(raw) if raw and self.headers.get('content-type', '').startswith('application/json') else None
        s.requests.append((self.command, path, query, body, dict(self.headers)))
        if path.startswith('/api/') and self.headers.get('X-CLI-Token') != 'test-token': return self.respond(401, dict(message='No token'))
        project_match = re.fullmatch(r'/api/v2/rsys/projects/([^/]+)(?:/(.*))?', path)
        if project_match:
            name, rest = project_match.groups()
            project = s.projects.get(name)
            if not project: return self.respond(404, dict(message='No project'))
            if not rest: return self.respond(200, project)
            if rest in ['resolve', 'compatible-releases']:
                found = s.matches(name, query)
                return self.respond(200, found if rest == 'compatible-releases' else found[0]) if found else self.respond(404, dict(message='No compatible release'))
            if rest == 'latest': return self.legacy_latest(name)
            if rest == 'releases' and self.command == 'POST':
                version = re.sub(r'^v(?=\d)', '', body['version'])
                record = s.add_release(name, version, project.get('template', {}).get('revisionId', 2), prerelease=body['prerelease'])
                return self.respond(200, record['release'])
            match = re.fullmatch(r'releases/([^/]+)(?:/(.*))?', rest)
            if match:
                version, action = match.groups()
                record = s.releases.get((name, re.sub(r'^v(?=\d)', '', version)))
                if not record: return self.respond(404, dict(message='No release'))
                if not action:
                    if self.command == 'PATCH': record['release'].update(body)
                    return self.respond(200, record['release'])
                if action == 'variants':
                    if self.command == 'PUT':
                        if record['token'] != body['expectedVersion']: return self.respond(409, dict(message='Stale variants'))
                        record['variants'] = [dict(v, automatic=False) for v in body['variants']]
                        record['token'] += 1
                    return self.respond(200, s.metadata(record))
                if action == 'files' and self.command == 'POST':
                    if body['filename'] == s.fail_create:
                        s.fail_create = ''
                        return self.respond(409, dict(message='Interrupted upload'))
                    file = s.add_file(record, body['filename'], b'')
                    file.update(body, status='UPLOADING', partCount=max(1, (body['size'] + file['partSize'] - 1) // file['partSize']))
                    return self.respond(200, file)
                if action == 'publish':
                    if not record['variants'] and not SCHEMAS[record['revision']]:
                        ready = [f for f in record['release']['files'] if f['status'] == 'READY']
                        s.add_variant(record, {}, ready, next((f for f in ready if f['primary']), ready[0]), automatic=True)
                        record['token'] += 1
                    for v in record['variants']:
                        if any(f['required'] and f['key'] not in v['selection'] for f in SCHEMAS[record['revision']]):
                            return self.respond(409, dict(message='Missing required fields'))
                    if not record['variants']: return self.respond(409, dict(message='No compatible builds'))
                    record['release']['status'] = 'PUBLISHED'
                    return self.respond(200, record['release'])
        file_match = re.fullmatch(r'/api/v2/rsys/files/(\d+)(?:/(.*))?', path)
        if file_match:
            identity, action = file_match.groups()
            stored = s.files.get(int(identity))
            if not stored: return self.respond(404, dict(message='No file'))
            file, record = stored['file'], stored['record']
            if self.command == 'DELETE':
                if record['release']['status'] == 'PUBLISHED' and any(v['defaultFileId'] == file['id'] and not v['automatic'] for v in record['variants']):
                    return self.respond(409, dict(message='Assign another default or yank before deleting'))
                changed = False
                for v in record['variants']:
                    if file['id'] in v['fileIds']:
                        changed = True
                        v['fileIds'].remove(file['id'])
                        if v['defaultFileId'] == file['id']: v['defaultFileId'] = None
                if changed: record['token'] += 1
                record['release']['files'].remove(file)
                del s.files[file['id']]
                return self.respond(200, {})
            if action and action.startswith('parts/'):
                mime = BytesParser(policy=email_policy).parsebytes(('Content-Type: ' + self.headers['Content-Type'] + '\r\nMIME-Version: 1.0\r\n\r\n').encode() + raw)
                payload = next(part.get_payload(decode=True) for part in mime.iter_parts() if part.get_param('name', header='content-disposition') == 'file')
                stored.setdefault('parts', {})[int(action.split('/')[1])] = payload
                stored['content'] = b''.join(stored['parts'][number] for number in sorted(stored['parts']))
                return self.respond(200, {})
            if action == 'complete':
                file['status'] = 'READY'
                file['primary'] = not any(f['primary'] for f in record['release']['files'] if f is not file)
                s.refresh(record)
                if s.concurrent:
                    s.concurrent = False
                    record['token'] += 1
                    s.add_variant(record, {'platform': 'spigot', 'minecraft_version': '1.21.2'}, [file], key='other-editor')
                return self.respond(200, file)
            if action == 'primary':
                if not file['primary']:
                    for f in record['release']['files']: f['primary'] = f is file
                    s.refresh(record)
                return self.respond(200, file)
            if action == 'download': return self.respond(200, stored['content'])
        match = re.fullmatch(r'/([^/]+)/public/(.*)', path)
        if match:
            name, rest = match.groups()
            if name not in s.projects or not s.projects[name]['isPublic']: return self.respond(404, dict(message='No project'))
            if rest in ['resolve.json', 'compatible-releases.json']:
                found = s.matches(name, query)
                if rest == 'compatible-releases.json': return self.respond(200, found)
                return self.respond(200, found[0]) if found else self.respond(404, dict(message='No compatible release'))
            if rest == 'latest.json': return self.legacy_latest(name)
            match = re.fullmatch(r'v/([^/]+)/(.+)', rest)
            if match:
                version, filename = match.groups()
                record = s.releases.get((name, version))
                if record:
                    file = next((f for f in record['release']['files'] if f['filename'] == filename), None)
                    if file: return self.respond(200, s.files[file['id']]['content'])
        return self.respond(404, dict(message='No route: ' + path))

    def legacy_latest(self, project):
        s = self.server.state
        found = [r for (p, _), r in s.releases.items() if p == project and r['release']['status'] == 'PUBLISHED']
        if not found: return self.respond(404, dict(message='No release'))
        r = found[-1]
        body = copy.deepcopy(r['release'])
        body['files'] = [dict(s.public_file(r, f), primary=f['primary']) for f in r['release']['files']]
        return self.respond(200, body)

    def respond(self, status, body):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/octet-stream' if isinstance(body, bytes) else 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = handle_request


class RsysTests(unittest.TestCase):
    binary = ''

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.state = State()
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.state = self.state
        self.state.base = 'http://127.0.0.1:' + str(self.server.server_port)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temp.cleanup()

    def run_cli(self, *args, token=True, success=True):
        env = dict(os.environ, GN_CLI_CONFIG_DIR=str(self.root / 'config'), GN_ACCEPT_TERMS='1', CI='true', GN_TOKEN='test-token' if token else '')
        result = subprocess.run([self.binary, '--no-unicode', '--no-color', '--noninteractive', '--rsys-url', self.state.base, *args],
                                env=env, cwd=self.root, capture_output=True, text=True, timeout=30)
        if success: self.assertEqual(0, result.returncode, result.stderr + result.stdout)
        else: self.assertNotEqual(0, result.returncode, result.stdout)
        return result

    def local(self, name, content=b'build'):
        (self.root / name).write_bytes(content)
        return name

    def manifest(self, variants):
        (self.root / 'variants.json').write_text(json.dumps(dict(variants=variants)))
        return 'variants.json'

    def compatibility(self, platform='paper', version='1.21.1'):
        return ['--compat', 'platform='+platform, '--compat=minecraft_version='+version]

    def writes(self): return [r for r in self.state.requests if r[0] != 'GET']

    def seed_build(self, project='plugin', version='1', platform='paper', minecraft='1.21.1', prerelease=False):
        r = self.state.add_release(project, version, status='PUBLISHED', prerelease=prerelease)
        extra = self.state.add_file(r, 'notes.txt', b'notes', primary=True)
        jar = self.state.add_file(r, platform+'.jar', (platform+' build').encode())
        self.state.add_variant(r, dict(platform=platform, minecraft_version=minecraft), [extra, jar], jar)
        return r, jar

    def test_publish_single_combination_before_publish_and_resume_same_files(self):
        file = self.local('plugin.jar')
        self.run_cli('rsys', 'publish', 'plugin', 'v2', file, *self.compatibility())
        r = self.state.releases[('plugin', '2')]
        self.assertEqual('PUBLISHED', r['release']['status'])
        self.assertEqual({'platform': 'paper', 'minecraft_version': '1.21.1'}, r['variants'][0]['selection'])
        paths = [request[1] for request in self.writes()]
        self.assertLess(next(i for i, p in enumerate(paths) if p.endswith('/variants')), next(i for i, p in enumerate(paths) if p.endswith('/publish')))
        self.state.requests.clear()
        self.run_cli('rsys', 'publish', 'plugin', '2', file, '--append', *self.compatibility())
        self.assertFalse(any('/parts/' in r[1] or r[1].endswith('/files') for r in self.writes()))

    def test_rejects_invalid_metadata_before_creating_release_or_uploading(self):
        file = self.local('plugin.jar')
        cases = [[], ['--compat', 'platform=paper'], self.compatibility('unknown'), self.compatibility(version='retired'),
                 [*self.compatibility(), '--compat', 'platform=spigot'], ['--compat', '=bad']]
        for flags in cases:
            with self.subTest(flags=flags):
                self.state.requests.clear()
                self.run_cli('rsys', 'publish', 'plugin', '2', file, *flags, success=False)
                self.assertEqual([], self.writes())

    def test_manifest_preserves_correlations_and_shares_uploaded_file(self):
        name = self.local('universal.jar')
        manifest = self.manifest([
            dict(key='paper', selection=dict(platform='paper', minecraft_version='1.21.1'), files=[name], defaultFile=name),
            dict(key='spigot', selection=dict(platform='spigot', minecraft_version='1.21.2'), files=[name], defaultFile=name)])
        self.run_cli('rsys', 'publish', 'plugin', '2', name, '--variants-file', manifest)
        r = self.state.releases[('plugin', '2')]
        self.assertEqual(1, len(r['release']['files']))
        self.assertEqual(2, len(r['variants']))
        self.assertEqual(r['variants'][0]['fileIds'], r['variants'][1]['fileIds'])
        self.run_cli('rsys', 'latest', 'plugin', *self.compatibility('paper', '1.21.2'), token=False, success=False)
        self.assertFalse(any(r[1].endswith('/latest.json') for r in self.state.requests))

    def test_manifest_rejects_duplicate_combinations_keys_and_missing_files(self):
        file = self.local('plugin.jar')
        item = dict(key='one', selection=dict(platform='paper', minecraft_version='1.21.1'), files=[file], defaultFile=file)
        cases = [[item, dict(item, key='two')], [item, dict(item, selection=dict(platform='spigot', minecraft_version='1.21.1'))],
                 [dict(item, files=['missing.jar'])], [dict(item, defaultFile='missing.jar')], [dict(item, selection=dict(platform=1))],
                 [dict(item, typo='bad')]]
        for variants in cases:
            with self.subTest(variants=variants):
                self.run_cli('rsys', 'publish', 'plugin', '2', file, '--variants-file', self.manifest(variants), success=False)
                self.assertEqual([], self.writes())
        (self.root/'variants.json').write_text('{"variants":[],"variants":[]}')
        self.run_cli('rsys', 'publish', 'plugin', '2', file, '--variants-file', 'variants.json', success=False)
        self.assertEqual([], self.writes())

    def test_draft_can_be_incomplete_and_completed_without_new_upload(self):
        file = self.local('plugin.jar')
        self.run_cli('rsys', 'publish', 'plugin', '2', file, '--draft', '--compat', 'platform=paper')
        self.assertEqual('DRAFT', self.state.releases[('plugin', '2')]['release']['status'])
        self.state.requests.clear()
        self.run_cli('rsys', 'publish', 'plugin', '2', *self.compatibility())
        self.assertFalse(any('/parts/' in r[1] for r in self.writes()))
        self.assertEqual('PUBLISHED', self.state.releases[('plugin', '2')]['release']['status'])

    def test_resumes_against_pinned_schema_after_project_revision_changes(self):
        r = self.state.add_release('plugin', '2')
        self.state.projects['plugin']['template'] = dict(revisionId=3, fields=SCHEMAS[3])
        self.run_cli('rsys', 'publish', 'plugin', '2', self.local('plugin.jar'), *self.compatibility())
        self.assertEqual(1, r['revision'])
        self.assertEqual('PUBLISHED', r['release']['status'])

    def test_concurrent_compatibility_edit_is_not_overwritten(self):
        self.state.concurrent = True
        result = self.run_cli('rsys', 'publish', 'plugin', '2', self.local('plugin.jar'), *self.compatibility(), success=False)
        self.assertIn('Compatibility changed during upload', result.stderr)
        self.assertEqual('DRAFT', self.state.releases[('plugin', '2')]['release']['status'])
        self.assertFalse(any(r[0] == 'PUT' for r in self.writes()))
        self.assertEqual('other-editor', self.state.releases[('plugin', '2')]['variants'][0]['key'])

    def test_interrupted_publish_keeps_files_and_skips_completed_files_on_retry(self):
        self.state.fail_create = 'two.jar'
        files = [self.local('one.jar', b'one'), self.local('two.jar', b'two')]
        self.run_cli('rsys', 'publish', 'plugin', '2', *files, *self.compatibility(), success=False)
        r = self.state.releases[('plugin', '2')]
        self.assertEqual('DRAFT', r['release']['status'])
        self.assertEqual(1, len(r['release']['files']))
        self.state.requests.clear()
        self.run_cli('rsys', 'publish', 'plugin', '2', *files, *self.compatibility())
        creates = [r[3]['filename'] for r in self.writes() if r[1].endswith('/files')]
        self.assertEqual(['two.jar'], creates)

    def test_public_resolution_uses_exact_default_checks_sha_and_sends_no_credentials(self):
        _, jar = self.seed_build()
        self.run_cli('rsys', 'download', 'plugin', *self.compatibility(), '-o', 'download.jar', token=False)
        self.assertEqual(b'paper build', (self.root/'download.jar').read_bytes())
        self.assertTrue(all('X-CLI-Token' not in r[4] for r in self.state.requests))
        result = self.run_cli('rsys', 'latest', 'plugin', *self.compatibility(), '--json', token=False)
        self.assertEqual(jar['filename'], json.loads(result.stdout)['primary']['filename'])
        self.run_cli('rsys', 'download', 'plugin', *self.compatibility(), '--file', 'spigot.jar', token=False, success=False)

    def test_private_resolution_downloads_with_auth_only_on_configured_server(self):
        self.seed_build('private')
        self.run_cli('rsys', 'download', 'private', *self.compatibility(), '-o', 'private.jar')
        self.assertEqual(b'paper build', (self.root/'private.jar').read_bytes())
        self.assertTrue(any(r[1].endswith('/download') and r[4].get('X-CLI-Token') == 'test-token' for r in self.state.requests))
        self.assertTrue(all('X-CLI-Token' not in r[4] for r in self.state.requests if '/public/' in r[1]))

    def test_prerelease_opt_in_and_compatible_since_history(self):
        self.seed_build(version='1')
        self.seed_build(version='2')
        self.seed_build(version='3-rc', prerelease=True)
        result = self.run_cli('rsys', 'latest', 'plugin', *self.compatibility(), '--since', '1', '--json', token=False)
        self.assertEqual('2', json.loads(result.stdout)['version'])
        self.assertEqual(['2'], [r['version'] for r in json.loads(result.stdout)['releases']])
        result = self.run_cli('rsys', 'latest', 'plugin', *self.compatibility(), '--prerelease', '--json', token=False)
        self.assertEqual('3-rc', json.loads(result.stdout)['version'])
        self.run_cli('rsys', 'latest', 'plugin', *self.compatibility(), '--since', 'missing', token=False, success=False)

    def test_no_match_never_falls_back_to_unfiltered_latest(self):
        self.seed_build()
        self.run_cli('rsys', 'download', 'plugin', *self.compatibility('spigot'), success=False)
        self.assertFalse(any('latest' in r[1] for r in self.state.requests))
        self.run_cli('rsys', 'download', 'plugin', '1', *self.compatibility(), success=False)

    def test_legacy_general_publish_download_and_same_file_retry_are_unchanged(self):
        for project in ['general', 'legacy']:
            file = self.local(project+'.zip', project.encode())
            self.run_cli('rsys', 'publish', project, '2', file)
            self.run_cli('rsys', 'publish', project, '2', file, '--append', '--draft')
            self.run_cli('rsys', 'download', project, '-o', project+'.out', token=False)
            self.assertEqual(project.encode(), (self.root/(project+'.out')).read_bytes())
        self.assertFalse(any(r[0] == 'PUT' for r in self.state.requests))

    def test_default_file_replacement_and_automatic_variant_append_use_fresh_tokens(self):
        r = self.state.add_release('plugin', '2')
        old = self.state.add_file(r, 'plugin.jar', b'old', primary=True)
        self.state.add_variant(r, dict(platform='paper', minecraft_version='1.21.1'), [old])
        self.run_cli('rsys', 'publish', 'plugin', '2', self.local('plugin.jar', b'new'), '--force', *self.compatibility())
        self.assertNotEqual(old['id'], r['variants'][0]['defaultFileId'])
        self.run_cli('rsys', 'publish', 'general', '1', self.local('one.zip', b'one'))
        manifest = self.manifest([dict(key='general', selection={}, files=['one.zip', 'two.zip'], defaultFile='two.zip')])
        self.run_cli('rsys', 'publish', 'general', '1', self.local('two.zip', b'two'), '--append', '--variants-file', manifest)
        general = self.state.releases[('general', '1')]
        self.assertFalse(general['variants'][0]['automatic'])
        self.assertEqual(2, len(general['variants'][0]['fileIds']))

    def test_multipart_upload_and_download_preserve_all_bytes(self):
        content = b'abcdefghi' * (1024 * 1024)
        self.run_cli('rsys', 'publish', 'general', '2', self.local('large.zip', content))
        self.run_cli('rsys', 'download', 'general', '-o', 'large.out', token=False)
        self.assertEqual(content, (self.root/'large.out').read_bytes())
        self.assertEqual(2, len([r for r in self.writes() if '/parts/' in r[1]]))

    def test_manifest_can_map_existing_ready_files_and_renamed_uploads(self):
        r = self.state.add_release('plugin', '2')
        old = self.state.add_file(r, 'notes.txt', b'notes', primary=True)
        local = self.local('local.jar')
        manifest = self.manifest([dict(key='build', selection=dict(platform='paper', minecraft_version='1.21.1'),
                                       files=['notes.txt', 'plugin.jar'], defaultFile='plugin.jar')])
        self.run_cli('rsys', 'publish', 'plugin', '2', local+'#plugin.jar', '--variants-file', manifest)
        self.assertIn(old['id'], r['variants'][0]['fileIds'])
        self.assertEqual('plugin.jar', self.state.files[r['variants'][0]['defaultFileId']]['file']['filename'])

    def test_old_server_accepts_flag_free_publish_but_rejects_compatibility(self):
        self.run_cli('rsys', 'publish', 'legacy', '1', self.local('legacy.zip'), *self.compatibility(), success=False)
        self.assertEqual([], self.writes())

    def test_checksum_mismatch_does_not_leave_a_corrupt_download(self):
        _, jar = self.seed_build()
        jar['sha256'] = '0'*64
        self.run_cli('rsys', 'download', 'plugin', *self.compatibility(), '-o', 'bad.jar', token=False, success=False)
        self.assertFalse((self.root/'bad.jar').exists())


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True)
    args, rest = parser.parse_known_args()
    RsysTests.binary = str(Path(args.binary).resolve())
    unittest.main(argv=[__file__, *rest])
