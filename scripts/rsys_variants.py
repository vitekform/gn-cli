#!/usr/bin/env python3
"""Generate the complete gn-cli manifest after all platform upload jobs finish."""
import json
import re
import sys


def manifest(release, require_complete=True):
    variants = []
    seen = set()
    for file in release['files']:
        if file['status'] != 'READY':
            raise ValueError('Unfinished upload: ' + file['filename'])
        name = file['filename']
        match = re.fullmatch(r'gn-cli-(linux|macos|windows)-(x64|arm64)(\.exe)?', name)
        if not match:
            raise ValueError('Unrecognized CLI artifact: ' + name)
        os_name, architecture, extension = match.groups()
        if bool(extension) != (os_name == 'windows'):
            raise ValueError('Unexpected executable extension: ' + name)
        key = f'{os_name}-{architecture}'
        if key in seen:
            raise ValueError('Duplicate platform: ' + key)
        seen.add(key)
        variants.append(dict(key=key, selection=dict(os=os_name, architecture=architecture), files=[name], defaultFile=name))
    if require_complete and not {'linux-x64', 'windows-x64'} <= seen:
        raise ValueError('Missing Linux x64 or Windows x64 build')
    if not variants:
        raise ValueError('No CLI binaries supplied')
    return dict(variants=variants)


if __name__ == '__main__':
    try:
        print(json.dumps(manifest(json.load(sys.stdin)), indent=2))
    except (ValueError, KeyError) as error:
        sys.exit(str(error))
