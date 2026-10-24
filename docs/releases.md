# Releases

Continuous delivery publishes tested firmware files to GitHub Releases. Flashing
a physical board remains a separate operation.

## Prepare a release

1. Set `ANALYZER_FIRMWARE_VERSION` in `core/analyzer_identity.h` and check that
   the documentation matches the release's capabilities.
2. Run `make docker-build` and inspect `out/ci/reports/summary.md`.
3. Complete [hardware validation](hardware-validation.md) for a release intended
   for board users. QEMU does not cover every physical behavior.
4. Commit and push the changes to the main branch. Wait for a successful workflow.
5. Create an annotated tag on the tested commit and push it:

```sh
# Example: choose the next version number for the project.
git tag -a v0.1.0 -m "Release v0.1.0"
git push origin v0.1.0
```

Pushing the tag explicitly requests publication. Use a suffix such as
`v0.1.0-rc.1` for a prerelease.

## Publication workflow

The tag starts a full build and test run in Docker. The release job downloads
the artifacts from that same run, verifies their SHA256 checksums and source
commit in `build-info.json`, then creates a GitHub Release containing BIN, HEX,
ELF, MAP, build metadata and checksums. GitHub generates the release notes.
Review them before announcing the release: users need its capabilities, board
limitations and hardware test results.

The release job uses the tested firmware files without rebuilding them. It does
not change the tag or replace an existing release. Rerunning it after successful
publication fails when it attempts to create the existing release, protecting
published files from silent replacement. Publish corrections under a new version
number. If publication fails partway through, inspect the release and its assets
before repairing it manually.

## Repository settings

- Enable GitHub Actions. Organization policy must allow the release job to
  request `contents: write` for the standard `GITHUB_TOKEN`.
- Protect the main branch and require the
  **Docker / native / QEMU / firmware** check.
- Restrict creation and modification of `v*` tags to release maintainers.
- Pull requests and manual `workflow_dispatch` runs cannot publish releases;
  the release job condition excludes them.

The workflow does not change repository settings or branch protection. The
repository owner configures them separately.

## Verify downloaded files

Download the release assets into one directory, then run:

```sh
sha256sum --check SHA256SUMS       # Linux
shasum -a 256 --check SHA256SUMS   # macOS
```

The BIN file is loaded at `0x08000000`. The ELF file includes debug symbols.
Check the board and clock requirements in the [quick start](../README.md)
before flashing.
