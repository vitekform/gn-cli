# Changelog

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
