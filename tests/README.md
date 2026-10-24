# RSYS CLI tests

Retired stock commands are also checked against the full executable:

```sh
python3 tests/test_stock_sunset.py --binary build/gn-cli -v
```

This checks all former subcommands and aliases in text and JSON modes with
an empty config directory and an unreachable local API URL. They must print
the sunset notice without authentication, terms prompts or network requests.

The suite runs a binary against a disposable local HTTP server. It uses fake tokens,
temporary config/download directories, and no production services. It checks exact
matching, manifests and shared files, early validation, interrupted uploads, schema
pinning, stale writes, private downloads, checksums, multipart bytes, and old general
publishing behavior. The Linux leg of `gn-cli.yml` runs it against the full executable:

```sh
python3 tests/test_rsys.py --binary build/gn-cli -v
```

For focused Linux development with a C++20 compiler, libcurl development files and
nlohmann/json headers installed:

```sh
python3 tests/build_rsys_driver.py --build-dir /tmp/rsys-cli-tests
python3 tests/test_rsys.py --binary /tmp/rsys-cli-tests/gn-cli-rsys-test -v
```

If JSON headers are in another directory, add `--json-include /path/to/include`.
The focused entry point includes the production parser and links the production
RSYS handlers and transport. Function-section linking discards unrelated entry
points and commands. It does not package the application or embed its notices.
Use the normal build workflow for platform packaging and distribution checks.

After deployment, check a public project without credentials or writes. Supply its
exact selectors (and `--prerelease` for prerelease-only projects):

```sh
python3 tests/check_rsys_rollout.py --project gn-cli --compat os=linux --compat architecture=x64
```

The parallel gn-cli publisher generates its complete platform manifest with
`scripts/rsys_variants.py` in the final publish job. Its mapping checks run with:

```sh
python3 tests/test_publish_manifest.py -v
```
