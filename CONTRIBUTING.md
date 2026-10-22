# Contributing to gn-cli

Build instructions and the command reference are in [README.md](README.md).
The CLI is a C++20 HTTP client. Working on it does not require access to the
NextGen backend, production credentials or any other repository.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
python3 tests/test_rsys.py --binary build/gn-cli -v
python3 tests/test_publish_manifest.py -v
cmake --install build --prefix dist
./dist/bin/gn --version
./dist/bin/gncli --version
```

The RSYS suite starts a local mock server and uses temporary directories and
fake credentials. It checks publishing, compatibility selection, private
downloads, multipart uploads and checksums without contacting production.
[tests/README.md](tests/README.md) describes focused builds and optional public
rollout checks. Linux, Windows and macOS packaging is checked by CI.

## API changes

Keep released clients working. Deploy additive backend changes before releasing
commands that need them. A command requiring a new CLI-token scope or route
must have the corresponding backend support deployed first; adding a command
here does not grant server permissions. Cover changed RSYS behavior with the
local mock-server suite. Other command changes should include a reproducible
verification procedure using a development server (`--api-url`).

Update command help, shell completion and the README when commands or options
change. Bump the version in `CMakeLists.txt` once per release: patch for fixes,
build/CI changes and docs; minor for new commands or options.

## Embedded texts

The binary embeds `LICENSE`, `THIRD-PARTY-NOTICES` and `TERMS.en.md` from this
repository. Builds never fetch current service terms from the network. When
the service terms change, update the versioned copy deliberately and bump the
CLI version. Update the third-party notices when bundled dependencies change.
The service terms apply to the hosted services; the source licence describes
rights to the CLI code.

Do not include account credentials or user data in issues, patches or logs.

## Modified builds and contributions

Modified builds must comply with the additional User-Agent condition in
[LICENSE](LICENSE). Configure `-DGN_CLI_USER_AGENT_PRODUCT=gn-cli_fork` or a
custom client name, and use a separate `GN_CLI_CONFIG_DIR` and login so an
official client's saved session cannot supply its old User-Agent.

By submitting a contribution, you agree to license it under this repository's
MIT License with the additional User-Agent condition. Retain existing
copyright and contributor notices.
