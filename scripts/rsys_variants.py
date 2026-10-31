#!/usr/bin/env python3
"""Generate the complete gn-cli manifest after all platform upload jobs finish."""
import json
import re
import sys


# Every platform CI builds (.github/workflows/gn-cli.yml); a release must have all of them.
# Each architecture has to be a choice of the project's rsys template, or publishing fails.
PLATFORMS = {
    'linux-x64', 'linux-arm64', 'linux-armv7', 'linux-riscv64', 'linux-ppc64le', 'linux-s390x',
    'macos-arm64', 'macos-x64',
    'windows-x64', 'windows-arm64', 'windows-x86',
}


def manifest(release, require_complete=True):
    variants = []
    seen = set()
    for file in release['files']:
        if file['status'] != 'READY':
            raise ValueError('Unfinished upload: ' + file['filename'])
        name = file['filename']
        match = re.fullmatch(r'gn-cli-(linux|macos|windows)-([a-z0-9]+)(\.exe)?', name)
        if not match or f'{match[1]}-{match[2]}' not in PLATFORMS:
            raise ValueError('Unrecognized CLI artifact: ' + name)
        os_name, architecture, extension = match.groups()
        if bool(extension) != (os_name == 'windows'):
            raise ValueError('Unexpected executable extension: ' + name)
        key = f'{os_name}-{architecture}'
        if key in seen:
            raise ValueError('Duplicate platform: ' + key)
        seen.add(key)
        variants.append(dict(key=key, selection=dict(os=os_name, architecture=architecture), files=[name], defaultFile=name))
    if require_complete and not PLATFORMS <= seen:
        raise ValueError('Missing build(s): ' + ', '.join(sorted(PLATFORMS - seen)))
    if not variants:
        raise ValueError('No CLI binaries supplied')
    return dict(variants=variants)


if __name__ == '__main__':
    try:
        print(json.dumps(manifest(json.load(sys.stdin)), indent=2))
    except (ValueError, KeyError) as error:
        sys.exit(str(error))
