# Changelog

## 1.15.2

- The 1.15.1 changes, released: 1.15.1's ppc64le build hung on a stalled Ubuntu mirror and
  was never published. The cross builds now time out and retry a stalled apt download.

## 1.15.1

- `gn <command> --help` for logs, memory, web, tools/tool, chat, files, me and credits adds a
  Details block: every subcommand with its parameters and defaults, the shape of the output
  (field names), and examples that work as written. `gn logs --help` includes the `where`
  query language and says that a search looks back one hour unless told otherwise. Each page
  stays under 4,000 characters, so the AI sandbox shows it whole.

## 1.15.0

- `gn <command> --help` and `gn help <command>` show only that command's lines, its options and
  its section heading (`gn logs --help`: about 300 characters instead of the whole 15,000-character
  help). An unknown word, and `gn --help` alone, still print everything. In the AI sandbox, whose
  output is cut to a few thousand characters, the whole help used to lose its middle.
- The `logs` help points at `gn tools logs_search` for the parameters and the query language.

## 1.14.0

- `gn tools`, `gn tool <name> key=value…`: the AI's read-only tools from the terminal, the same
  ones a chat calls (needs the backend's `/api/v2/ai/tools`). `gn tools <name>` shows a tool's
  parameters, from the server.
- Short forms: `gn logs search|count|trace|sources|booru`, `gn memory list|read|read-chat`,
  `gn web search|fetch|research`. The memory commands run in a chat: `--chat <id>`, or
  `$GN_SANDBOX_CHAT` in the AI sandbox.
- CLI token scopes for them: `logs:read`, `memory:read`, `web:search`.

## 1.13.1

- Remove `gn khk`. KHK's membership applications moved off ganamaga.me to their own service
  with its own accounts, so its commands, the `khk:admin` token scope and the `khkAdmin`
  permission id reach nothing there any more.

## 1.13.0

- New builds: Linux armv7 (`gn-cli-linux-armv7`, 32-bit Raspberry Pi OS and other
  armhf systems), riscv64, ppc64le and s390x, cross-compiled and tested under QEMU.
- macOS is now one universal binary for Apple Silicon and Intel, published as both
  `gn-cli-macos-arm64` and `gn-cli-macos-x64`, and runs on macOS 11 and newer.
- Windows x86 (32-bit) build: `gn-cli-windows-x86.exe`.
- `gn-cli update` picks the right build on all of these; Homebrew installs on Intel Macs.
- Every platform builds on every push and pull request, macOS included.

## 1.12.1

- Build and publish Linux arm64 (`gn-cli-linux-arm64`) and Windows arm64
  (`gn-cli-windows-arm64.exe`) binaries on GitHub's native arm64 runners.
- `gn-cli update` picks the arm64 build on arm64 Linux and Windows.
- The Homebrew formula installs on arm64 Linux too.

## 1.11.2

- Move gn-cli into its standalone public repository.
- Build with a versioned local copy of the service terms, without a frontend checkout.
- Run CLI tests and the Linux, Windows and macOS release workflows from the repository root.
- License source and builds under MIT with the additional fork User-Agent condition.
- Allow configuring the HTTP User-Agent product at build time.
- Preserve rsys downloads, update filenames, compatibility selectors and the Homebrew tap.

Earlier release notes remain at https://ganamaga.me/releases/gn-cli.
