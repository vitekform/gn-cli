#!/usr/bin/env python3
"""Read-only public deployment check; no credentials or writes."""
import argparse
import json
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlencode
from urllib.request import Request, urlopen

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--base-url', default='https://rsys.ganamaga.me')
parser.add_argument('--project', default='gn-cli')
parser.add_argument('--compat', action='append', default=[], metavar='FIELD=VALUE')
parser.add_argument('--prerelease', action='store_true')
args = parser.parse_args()
selection = {}
for value in args.compat:
    key, separator, choice = value.partition('=')
    if not separator or not key or not choice or key in selection:
        parser.error('--compat needs one FIELD=VALUE per distinct field')
    selection[key] = choice
base = args.base_url.rstrip('/')
prefix = base + '/' + quote(args.project, safe='') + '/public/'
parameters = {'f.' + key: value for key, value in selection.items()}
if args.prerelease:
    parameters['prerelease'] = 'true'
query = '?' + urlencode(parameters) if parameters else ''


def get(endpoint):
    request = Request(prefix + endpoint, headers={'Accept': 'application/json', 'Cache-Control': 'no-cache', 'User-Agent': 'RSYS-rollout-check'})
    try:
        with urlopen(request, timeout=30) as response:
            if 'application/json' not in response.headers.get('Content-Type', ''):
                raise RuntimeError(endpoint + ' returned a non-JSON response (check deployment and host/proxy routing)')
            return json.load(response)
    except HTTPError as error:
        raise RuntimeError(endpoint + ' returned HTTP ' + str(error.code)) from error


try:
    catalog = get('project.json' + ('?prerelease=true' if args.prerelease else ''))
    assert catalog.get('slug') == args.project, 'Catalog project does not match'
    assert selection in catalog.get('selections', []), 'No published download for this exact selection'
    resolved = get('resolve.json' + query)
    history = get('compatible-releases.json' + query)
    assert resolved.get('selection') == selection, 'Resolved selection differs'
    release = get('v/' + quote(resolved['version'], safe='') + '.json')
    primary = next(f for f in release['files'] if f['filename'] == resolved['primary']['filename'])
    assert primary['sha256'] == resolved['primary']['sha256'], 'Resolved file checksum differs from release metadata'
    assert any(r['version'] == resolved['version'] for r in history), 'Resolved release is absent from compatible history'
    if not selection:
        latest = get('latest.json' + ('?prerelease=true' if args.prerelease else ''))
        assert resolved['version'] == latest['version'], 'Latest changed between requests; retry the check'
        legacy_primary = latest.get('primary') or next((f for f in latest['files'] if f.get('primary')), latest['files'][0])
        assert resolved['primary']['filename'] == legacy_primary['filename'], 'Compatible and global defaults differ'
    print('Verified', args.project, resolved['version'], selection, ': catalog, compatible latest/history, and file checksum agree')
except (AssertionError, RuntimeError, ValueError, KeyError, StopIteration, URLError) as error:
    parser.exit(1, 'RSYS rollout check failed: ' + str(error) + '\n')
