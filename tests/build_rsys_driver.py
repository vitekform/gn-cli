#!/usr/bin/env python3
"""Build the focused Linux RSYS test entry point without packaging the full application."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import re
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', required=True)
parser.add_argument('--json-include', help='Directory containing nlohmann/json.hpp, if not installed system-wide')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
build = Path(args.build_dir).resolve()
build.mkdir(parents=True, exist_ok=True)
version = re.search(r'project\(gn_cli VERSION ([\d.]+)', (root/'CMakeLists.txt').read_text()).group(1)
compiler = shlex.split(os.environ.get('CXX', 'g++'))
flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'libcurl'], text=True))
if args.json_include: flags += ['-I'+str(Path(args.json_include).resolve())]
sources = [root/'tests/rsys_driver.cpp'] + [root/'src'/f'{name}.cpp' for name in ['commands', 'rsys_compatibility', 'common', 'api', 'config', 'http', 'term', 'sha256']]


def compile_source(source):
    target = build/(source.stem+'.o')
    subprocess.run([*compiler, '-std=c++20', '-O0', '-g', '-Wall', '-Wextra', '-Wpedantic', '-ffunction-sections', '-fdata-sections',
                    f'-DGN_CLI_VERSION="{version}"', '-I'+str(root/'src'), *flags, '-c', str(source), '-o', str(target)], check=True)
    return target


with ThreadPoolExecutor(max_workers=2) as pool:
    objects = list(pool.map(compile_source, sources))
libs = shlex.split(subprocess.check_output(['pkg-config', '--libs', 'libcurl'], text=True))
subprocess.run([*compiler, '-Wl,--gc-sections', *map(str, objects), *libs, '-o', str(build/'gn-cli-rsys-test')], check=True)
print(build/'gn-cli-rsys-test')
