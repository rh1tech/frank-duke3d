# CI/CD Pipeline Documentation

## Overview

This project uses a **self-hosted GitHub Actions runner** named `rp2350-builder` to build firmware releases. The runner lives on a dedicated Debian x86_64 build server and is shared across multiple RP2350-based repositories under the `rh1tech` GitHub account.

When a commit message starts with `release:` followed by a version number, the pipeline automatically builds all 4 firmware variants and publishes them as a GitHub Release.

## Architecture

```
Developer Machine                  GitHub                     Build Server
┌──────────────┐              ┌──────────────┐           ┌─────────────────────┐
│ git commit   │──── push ───>│ Actions      │── job ──> │ rp2350-builder      │
│ "release: X" │              │ workflow     │           │ (self-hosted)       │
└──────────────┘              │ triggered    │           │                     │
                              └──────┬───────┘           │ ~/pico-sdk/         │
                                     │                   │ ~/runners/          │
                              ┌──────▼───────┐           │   ├── frank-duke3d/   │
                              │ GitHub       │<── .uf2 ──│   ├── other-repo/   │
                              │ Release      │   .m1p2   │   └── ...           │
                              │ created      │   .m2p2   └─────────────────────┘
                              └──────────────┘
```

### Build server details

| Property | Value |
|----------|-------|
| Host | `rbx1.re-hash.org` |
| User | `xtreme` |
| OS | Debian 12 (Bookworm), x86_64 |
| Runner name | `rp2350-builder` |
| Runner labels | `self-hosted`, `linux`, `rp2350` |
| Pico SDK path | `/home/xtreme/pico-sdk` |
| Runner instances | `/home/xtreme/runners/<repo_name>/` |
| Service name pattern | `actions.runner.<owner>-<repo>.rp2350-builder.service` |

### Installed build toolchain

- `gcc-arm-none-eabi` — ARM cross-compiler for RP2350
- `cmake` — build system
- `libnewlib-arm-none-eabi` — C standard library for ARM
- `libstdc++-arm-none-eabi-newlib` — C++ standard library for ARM
- `pico-sdk` (cloned at `~/pico-sdk` with submodules)

## Triggering a Release

Commit with a message starting with `release:` followed by a version in `MAJOR.MINOR` format:

```bash
git commit -m "release: 1.04"
git push
```

### Accepted commit message formats

| Format | Parsed version | Valid |
|--------|---------------|-------|
| `release: 1.04` | 1.04 | ✅ |
| `release:1.04` | 1.04 | ✅ |
| `release: 2.00` | 2.00 | ✅ |
| `release: 1.04 - fixed audio` | 1.04 | ✅ (text after version ignored) |
| `Release: 1.04` | — | ❌ (case-sensitive) |
| `release: 1.4` | 1.04 | ✅ (minor zero-padded automatically) |
| `fix: something` | — | ❌ (not a release, workflow skipped) |

### Version rules

- **Major**: integer ≥ 1
- **Minor**: integer 0–99 (displayed zero-padded as two digits)
- Version is stored in `version.txt` as `MAJOR MINOR` (space-separated)

## What the Pipeline Does

When triggered by a `release:` commit on the `main` or `master` branch:

1. **Checks out** the repository with submodules
2. **Parses** the version from the first line of the commit message
3. **Runs `release-ci.sh`** which builds all 4 firmware variants:

| # | File pattern | Board | CPU | PSRAM |
|---|-------------|-------|-----|-------|
| 1 | `frank-duke3d_m1_378_133_*.uf2` | M1 | 378 MHz | 133 MHz |
| 2 | `frank-duke3d_m1_504_166_*.uf2` | M1 | 504 MHz | 166 MHz |
| 3 | `frank-duke3d_m2_378_133_*.uf2` | M2 | 378 MHz | 133 MHz |
| 4 | `frank-duke3d_m2_504_166_*.uf2` | M2 | 504 MHz | 166 MHz |

4. **Creates a GitHub Release** tagged `vMAJOR.MINOR` with all firmware files attached

### CMake flags used per build

```
-DBOARD_VARIANT=<M1|M2>
-DCPU_SPEED=<378|504>
-DPSRAM_SPEED=<133|166>
-DFLASH_SPEED=66
-DUSB_HID_ENABLED=ON
-DMOS2=<ON|OFF>
```

### Output filename convention

```
frank-duke3d_m<BOARD>_<CPU>_<PSRAM>_<MAJOR>_<MINOR>.<ext>
```

- `BOARD` — `1` (M1) or `2` (M2)
- `CPU` — CPU clock in MHz
- `PSRAM` — PSRAM clock in MHz
- `MAJOR_MINOR` — version with minor zero-padded (e.g., `1_04`)
- `ext` — `uf2`

## Files

| File | Purpose |
|------|---------|
| `.github/workflows/release.yml` | GitHub Actions workflow definition |
| `release-ci.sh` | Non-interactive release build script (called by CI) |
| `release.sh` | Interactive release build script (for local manual builds) |
| `setup-runner.sh` | Initial server setup (installs toolchain, Pico SDK, first runner) |
| `add-runner.sh` | Adds the runner to additional repos (run on server) |
| `version.txt` | Current version as `MAJOR MINOR` |

## Workflow YAML Reference

The workflow file at `.github/workflows/release.yml`:

- **Trigger**: `push` to `main` or `master`
- **Condition**: `if: startsWith(github.event.head_commit.message, 'release:')`
- **Permissions**: `contents: write` (required to create releases and tags)
- **Runner**: `runs-on: [self-hosted, rp2350]`
- **Release action**: `softprops/action-gh-release@v2`
- **Auth**: Uses the automatic `GITHUB_TOKEN` secret (no manual token setup needed)

## Adding the Runner to Another Repository

The runner is designed to be shared across all RP2350-based projects. Each repo gets its own runner *instance* (separate work directory and systemd service) but shares the same Pico SDK and ARM toolchain.

### Steps

From your local machine with `gh` CLI authenticated:

```bash
# 1. Generate a registration token for the target repo
TOKEN=$(GH_PAGER=cat gh api --method POST repos/rh1tech/<REPO>/actions/runners/registration-token --jq .token)

# 2. SSH into the server and run the add script
ssh xtreme@rbx1.re-hash.org "~/add-runner.sh rh1tech/<REPO> $TOKEN"
```

### What `add-runner.sh` does

1. Creates `~/runners/<repo_name>/` on the server
2. Copies runner binaries from an existing instance (avoids re-downloading)
3. Registers with GitHub using name `rp2350-builder` and labels `self-hosted,linux,rp2350`
4. Sets `PICO_SDK_PATH=/home/xtreme/pico-sdk` in the runner's `.env` file
5. Installs and starts a systemd service for the runner

### Using the runner in another repo's workflow

```yaml
jobs:
  build:
    runs-on: [self-hosted, rp2350]
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive
      - run: |
          # PICO_SDK_PATH is set automatically via runner .env
          mkdir build && cd build
          cmake .. -DPICO_PLATFORM=rp2350
          make -j$(nproc)
```

## Server Administration

### Check runner status

```bash
ssh xtreme@rbx1.re-hash.org 'sudo systemctl status actions.runner.rh1tech-frank-duke3d.rp2350-builder.service'
```

### View runner logs

```bash
ssh xtreme@rbx1.re-hash.org 'journalctl -u actions.runner.rh1tech-frank-duke3d.rp2350-builder.service -f'
```

### Restart runner

```bash
ssh xtreme@rbx1.re-hash.org 'cd ~/runners/frank-duke3d && sudo ./svc.sh stop && sudo ./svc.sh start'
```

### List all runner services on the server

```bash
ssh xtreme@rbx1.re-hash.org 'systemctl list-units --type=service | grep actions.runner'
```

### Remove a runner from a repo

```bash
# Get a removal token
TOKEN=$(GH_PAGER=cat gh api --method POST repos/rh1tech/<REPO>/actions/runners/remove-token --jq .token)

# Remove on server
ssh xtreme@rbx1.re-hash.org "cd ~/runners/<REPO> && sudo ./svc.sh stop && sudo ./svc.sh uninstall && ./config.sh remove --token $TOKEN"
```

### Update Pico SDK

```bash
ssh xtreme@rbx1.re-hash.org 'cd ~/pico-sdk && git pull && git submodule update --init'
```

### Full server rebuild (from scratch)

If the server is reinstalled or you need to set up a new one:

```bash
scp setup-runner.sh xtreme@<new-server>:~/
ssh xtreme@<new-server> 'chmod +x setup-runner.sh && ./setup-runner.sh'
```

Then add each repo with `add-runner.sh`.

## Troubleshooting

### Workflow doesn't trigger

- Commit message must start with `release:` (lowercase, with colon)
- Push must be to `main` or `master` branch
- Check the Actions tab on GitHub for skipped runs

### Runner shows as offline

```bash
# Check service status
ssh xtreme@rbx1.re-hash.org 'cd ~/runners/frank-duke3d && sudo ./svc.sh status'

# Restart
ssh xtreme@rbx1.re-hash.org 'cd ~/runners/frank-duke3d && sudo ./svc.sh stop && sudo ./svc.sh start'
```

### Build fails with "PICO_SDK_PATH not set"

The runner's `.env` file may be missing or incorrect:

```bash
ssh xtreme@rbx1.re-hash.org 'cat ~/runners/frank-duke3d/.env'
# Should show: PICO_SDK_PATH=/home/xtreme/pico-sdk
```

Fix:

```bash
ssh xtreme@rbx1.re-hash.org 'echo "PICO_SDK_PATH=/home/xtreme/pico-sdk" > ~/runners/frank-duke3d/.env'
```

### Broken symlinks after runner auto-update

The GitHub runner auto-updates itself. If `bin` or `externals` become broken symlinks:

```bash
ssh xtreme@rbx1.re-hash.org 'cd ~/runners/frank-duke3d && ls -la bin externals'
# If broken, fix by pointing to latest versioned directory:
ssh xtreme@rbx1.re-hash.org 'cd ~/runners/frank-duke3d && rm -f bin externals && ln -s bin.* bin && ln -s externals.* externals'
```

### Version already exists on GitHub

If you push `release: 1.04` but tag `v1.04` already exists, the release step will fail. Either:
- Bump the version number, or
- Delete the existing release and tag on GitHub first
