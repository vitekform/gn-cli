# Publishing gn-cli

The canonical source is https://github.com/vitekform/gn-cli. The repository
contains only the CLI snapshot, its tests, build files and documentation;
private monorepo history is not part of this repository.

## Repository setup

Use `main` as the default branch. Both `main` and `master` are supported by
the workflows. Under Settings → Secrets and variables → Actions, configure:

- `RSYS_TOKEN`: a CLI token with `rsys:read` and `rsys:publish` from an account
  with the `release_system` permission. The existing rsys project is `gn-cli`;
  do not create a replacement project or change its public URLs.
- `HOMEBREW_TAP_DEPLOY_KEY_B64`: the existing Homebrew tap's SSH deploy key,
  or a replacement with write access to `vitekform/homebrew-tap`, encoded as
  base64. No production environment file is needed.

These secrets are repository-specific; creating the standalone repository
or moving workflows does not transfer them from the monorepo. Enter them in
GitHub Settings, never in source or an issue. The workflows use `GITHUB_TOKEN`
to tag, dispatch and create GitHub releases with explicit job permissions.
Pull-request builds do not publish or receive publishing credentials.

## Releases

Bump `project(gn_cli VERSION ...)` in `CMakeLists.txt` and push once. The tag
workflow creates `gn-cli-v<version>` if absent and dispatches `gn-cli.yml` on
that tag with `publish: true`. The build matrix uploads all platform binaries
to a draft; the publish job attaches the complete OS/architecture manifest
and publishes it after all builds succeed. Stable tags also create a GitHub
release and update the Homebrew formula. Preserve the `gn-cli-v` tag prefix.

Without `RSYS_TOKEN`, CI still builds and tests but skips publication. After
configuring secrets, use Actions → gn-cli → Run workflow, select the existing
`gn-cli-v<version>` tag and enable `publish`. Re-running the tag workflow
alone will not release an already-existing tag. Failed draft uploads can be
retried for that version; already-uploaded identical files are reused.

Published filenames are `gn-cli-linux-{x64,arm64,armv7,riscv64,ppc64le,s390x}`,
`gn-cli-macos-{arm64,x64}` (the same universal binary under both names) and
`gn-cli-windows-{x64,arm64,x86}.exe`. Linux and Windows arm64 arrived in 1.12.1,
the rest in 1.13.0.

Every architecture above must be a choice of the `architecture` field in the
gn-cli project's rsys template revision (x64, arm64, armv7, riscv64, ppc64le,
s390x, x86), or the publish job is refused with "Unknown choice for
architecture". Adding a platform therefore starts in the template editor: add
the choice, publish the revision and move the project to it, then push. The updater, public rsys project, compatibility
selectors and `brew install vitekform/tap/gn-cli` stay unchanged. Existing
releases remain hosted at rsys. Historical GitHub releases in the private
repository do not need to be copied.

## Monorepo cutover

After the standalone source is published and CI passes, remove `gn_cli/`
and the two gn-cli workflows from `vitekform/nextgen`, leaving a pointer to
this repository. Keep `ci/fetch_gn_cli.sh`: the Android publisher already
consumes the rsys binary and needs no source checkout or submodule.
The monorepo's own `RSYS_TOKEN` is still needed for Android publishing;
do not remove it. Transfer the gn-cli workflows before retiring the old
ones so there is always a working release path.
