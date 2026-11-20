# Signing the update file in CI (spec RS.18-RS.20, T33a part 4)

The Claude worker cannot edit `.github/workflows/`, so this page holds the
change for the user to apply by hand. Nothing here contains a secret.

## One-time steps for the user (RS.20)

1. On your own computer: `cd flipper && python3 tools/gzsign --new-key`.
   This writes `~/.config/galtzagorriak/update.key` (private, mode 0600)
   and `update.pub` (64 hex digits). Keep a copy of `update.key` offline
   (on a USB stick in a drawer, for example). If you lose it, no Flipper with
   your `atea` will accept a new release until it gets a new `atea` over DFU.
2. Commit the **public** key as `flipper/keys/release.pub`
   (`cp ~/.config/galtzagorriak/update.pub flipper/keys/release.pub`).
   `make update` then checks every file against it, and `atea` (T33b) puts
   it into slot 1.
3. On GitHub: Settings → Environments → New environment `release`. Under
   "Deployment protection rules", add yourself as a required reviewer. Under
   "Deployment branches and tags", allow only tags `v*`. Add the environment
   secret `GZ_UPDATE_KEY`, holding the whole text of `update.key` (from
   `-----BEGIN PRIVATE KEY-----` through `-----END PRIVATE KEY-----`).
   Because the secret belongs to that environment, PR jobs and
   `claude-worker.yml` never see it. Only a tag run that you approve does.

## The workflow change

Add this job to `.github/workflows/build.yml`, after `release`. It builds
again from the tag instead of reusing the artifact, because `make update`
needs the base package and the build directory. The output is reproducible
(part 1), so the signed file matches the images `release` attached.

```yaml
  # RS.18: the signed update file Galtzagorriak-X.Y.Z.update + .sha512.
  # GZ_UPDATE_KEY lives in the environment `release` (the user approves
  # each tag run); without it the job says so and attaches nothing.
  release-update:
    if: startsWith(github.ref, 'refs/tags/v')
    needs: [release]
    runs-on: ubuntu-24.04
    timeout-minutes: 20
    environment: release
    permissions:
      contents: write
    defaults:
      run:
        working-directory: flipper
    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 0
      - uses: ./.github/actions/toolchain
      - name: Version and epoch (RS.19)
        run: make versioncheck
      - name: Build, sign and check the update file
        id: upd
        env:
          GZ_UPDATE_KEY: ${{ secrets.GZ_UPDATE_KEY }}
        run: |
          if [ -z "$GZ_UPDATE_KEY" ]; then
            echo "::warning::GZ_UPDATE_KEY is not set in the environment" \
                 "'release': no update file for this release (see" \
                 "docs/devguide/release-update-job.md)"
            echo "skip=1" >> "$GITHUB_OUTPUT"
            exit 0
          fi
          test -f keys/release.pub || {
            echo "::error::flipper/keys/release.pub missing: commit the public key first"
            exit 1; }
          make BOARD=flipper -j"$(nproc)" minix card base-flash base pkgs
          make BOARD=flipper update
          v=$(sed -n 's/^#define GZ_RELEASE[^"]*"\(.*\)"/\1/p' \
                ../include/minix/gzversion.h)
          mkdir -p ../upd
          cp build/flipper/minix/Galtzagorriak.update \
             ../upd/Galtzagorriak-$v.update
          cd ../upd && sha512sum Galtzagorriak-$v.update \
             > Galtzagorriak-$v.update.sha512 && cat *.sha512
      - name: Attach to the release
        if: steps.upd.outputs.skip != '1'
        env:
          GH_TOKEN: ${{ github.token }}
          GH_REPO: ${{ github.repository }}
          TAG: ${{ github.ref_name }}
        run: gh release upload "$TAG" ../upd/* --clobber
```

Notes:
- `make update` refuses a file signed with a key other than
  `keys/release.pub` (or your own `update.pub`). A wrong secret therefore
  fails the job instead of publishing a file that no Flipper accepts.
- The private key exists only in the job's environment variable. `gzsign`
  reads it from `GZ_UPDATE_KEY` and never writes it to disk.
- The Python package `cryptography` is part of the `ubuntu-24.04` runner
  image (`python3-cryptography`), so nothing extra needs to be installed.
- `make versioncheck` (part of `ramcheck` and `make check`, so already in the
  `build` job) also checks RS.19: a new `GZ_RELEASE` since the newest tag
  `v*` needs a higher `GZ_EPOCH`, and an unchanged `GZ_RELEASE` needs the same
  `GZ_EPOCH`.
