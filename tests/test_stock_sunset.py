#!/usr/bin/env python3
"""Check retired stock commands locally, without terms acceptance or credentials."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', required=True)
args, unittest_args = parser.parse_known_args()
binary = str(Path(args.binary).resolve())


class StockSunsetTest(unittest.TestCase):
    def test_all_stock_commands_are_local_sunset_notices(self):
        commands = [[], ['aapl'], ['price', 'aapl'], ['quote', 'aapl'],
                    ['score', 'aapl', 'nvda'], ['news'], ['news', 'aapl'],
                    ['macro'], ['macro', 'FEDFUNDS'], ['geo'], ['geoevents', 'usa'],
                    ['tickers'], ['score']]
        for alias in ('stock', 'stocks'):
            for command in commands:
                for as_json in (False, True):
                    with self.subTest(alias=alias, command=command, json=as_json):
                        with tempfile.TemporaryDirectory() as config:
                            env = dict(os.environ, GN_CLI_CONFIG_DIR=config)
                            for key in ('GN_TOKEN', 'GN_ACCEPT_TERMS', 'GN_API_URL'):
                                env.pop(key, None)
                            # A closed local port makes any accidental HTTP request fail.
                            result = subprocess.run(
                                [binary, '--api-url', 'http://127.0.0.1:1',
                                 *(['--json'] if as_json else []), alias, *command],
                                env=env, input='', capture_output=True, text=True, timeout=5)
                            self.assertEqual(result.returncode, 1)
                            self.assertEqual(result.stderr, '')
                            if as_json:
                                self.assertEqual(json.loads(result.stdout), {
                                    'error': 'stock_sunset',
                                    'message': 'Stock system has been sunset',
                                    'url': 'https://ganamaga.me/blog/stock/sunset'})
                            else:
                                self.assertEqual(result.stdout,
                                    'Stock system has been sunset\n'
                                    'https://ganamaga.me/blog/stock/sunset\n')
                            self.assertEqual(list(Path(config).iterdir()), [])


if __name__ == '__main__':
    unittest.main(argv=[__file__, *unittest_args])
