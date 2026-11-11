<p align="center">
  <img src="resources/icons/steam-library-mover.svg" width="96" alt="Steam Library Mover icon">
</p>

<h1 align="center">Steam Library Mover</h1>

<p align="center">
  Move installed Steam games between Steam libraries on Linux — without Steam's
  <code>Move install folder</code> operation.
</p>

<p align="center">
  <a href="https://github.com/NasirA1991/steam-library-mover/actions/workflows/ci.yml">
    <img src="https://github.com/NasirA1991/steam-library-mover/actions/workflows/ci.yml/badge.svg"
         alt="CI status">
  </a>
  <a href="https://github.com/NasirA1991/steam-library-mover/releases">
    <img src="https://img.shields.io/github/v/release/NasirA1991/steam-library-mover?include_prereleases&amp;label=release"
         alt="Latest release">
  </a>
  <a href="https://aur.archlinux.org/packages/steam-library-mover">
    <img src="https://img.shields.io/aur/version/steam-library-mover?label=AUR"
         alt="AUR package">
  </a>
  <a href="LICENSE">
    <img src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg"
         alt="GPL-3.0-or-later">
  </a>
</p>

> **Early alpha.** Steam Library Mover moves real game files. Keep an independent backup of
> irreplaceable saves and game data. Steam must be closed while a move is running.

Steam Library Mover is for Linux gamers who need to move a game between drives or Steam libraries
when Steam's own mover fails — especially Proton games on NTFS drives, shared `compatdata`
symlinks, or libraries that contain filenames Windows filesystems cannot represent.

It reviews the move first, refuses unsafe destinations, journals every operation and keeps the
source intact until the destination has been verified and the Steam manifest has been updated.
The desktop app and CLI use the same move engine.

**Current public release:** v0.1.9 (alpha). See the [release notes](docs/RELEASE_NOTES.md) and
[roadmap decisions](docs/ROADMAP.md).

## Quick start

### Arch Linux, CachyOS or EndeavourOS

Install from the AUR with your preferred helper:

| Helper | Command |
| --- | --- |
| `yay` | `yay -S steam-library-mover` |
| `paru` | `paru -S steam-library-mover` |
| `pikaur` | `pikaur -S steam-library-mover` |

Any other AUR helper works too: `<helper> -S steam-library-mover`.

### AppImage

Download the latest portable build:

```bash
curl -LO https://github.com/NasirA1991/steam-library-mover/releases/latest/download/Steam_Library_Mover-x86_64.AppImage
chmod +x Steam_Library_Mover-x86_64.AppImage
./Steam_Library_Mover-x86_64.AppImage
```

### Other distributions

Use the AppImage above, or [build from source](docs/INSTALL.md). A Flatpak build is not currently
offered. See [Installation](docs/INSTALL.md) for dependencies, source builds and uninstall
instructions.

**Uninstall:** `sudo pacman -Rns steam-library-mover` for the AUR package, or delete the AppImage.

## Requirements

- Linux x86_64 with Steam installed.
- The destination Steam library must already exist and be registered in Steam. Steam Library Mover
  does not create libraries or add drive entries.
- Steam and any Proton/Wine games must be closed before a move. Scanning can run while Steam is
  open; the planner and executor re-check process state.
- The destination must be writable and have enough free space.
- Keep an independent backup of saves and irreplaceable data while the project is in alpha.

**On this page:** [Requirements](#requirements) · [Screenshots](#screenshots) ·
[How to move](#how-to-move-a-game) · [Features](#features) · [Safety](#safety) ·
[Limitations](#limitations) · [NTFS](#ntfs-libraries) · [Command line](#command-line) ·
[Troubleshooting](#troubleshooting) · [Documentation](#documentation)

## Screenshots

<p align="center"><b>Light theme</b></p>

<p align="center">
  <img src="docs/screenshots/main-window.png" alt="Steam Library Mover main window in the light theme" width="100%">
</p>

<p align="center"><b>Dark theme</b></p>

<p align="center">
  <img src="docs/screenshots/main-window-dark.png" alt="Steam Library Mover main window in the dark theme" width="100%">
</p>

<p align="center">
  <img src="docs/screenshots/transfer-review.png" alt="Transfer review" width="46%">
  &nbsp;
  <img src="docs/screenshots/transfer-progress.png" alt="Transfer progress" width="46%">
</p>

<p align="center"><sub>Screenshots use a demo library, not a real game collection. Review every move
before it starts; unsafe destinations are explained instead of being attempted.</sub></p>

## How to move a game

1. **Close Steam and all Proton/Wine games.**
2. Select the games you want to move using the checkboxes.
3. Choose the destination library in the transfer strip.
4. Click **Review transfer** and check the destination space, source libraries and any warnings.
5. Click **Start transfer**.

The source data is not deleted until the destination has been copied, verified and the Steam
manifest has been updated. If something fails before that point, the source remains usable.

## Features

### Move safely

- **Safe review before moving** — shows source and destination, free space, payload size and every
  blocker before any file is touched.
- **Full SHA-256 verification by default** — quick mode is an explicit opt-out and is labelled as
  unable to detect same-size corruption.
- **Proton-aware moves** — handles shared `compatdata` symlinks and avoids moving a prefix onto
  itself, the common cause of Steam's “content file locked” failure.
- **NTFS preflight** — detects Windows-invalid names, reserved names, case collisions and
  executable-bit risks before copying to NTFS/exFAT/FAT32.
- **Fast copies where possible** — uses copy-on-write reflinks on Btrfs/XFS, with a verified normal
  copy fallback elsewhere.

### Control and recovery

- **Journaled and recoverable** — every operation is recorded; interrupted copies can be resumed
  from the Recovery dialog, a failed verification is repaired once automatically, and unsafe
  states can be rolled back.
- **Single-writer safety** — a machine-wide lock prevents overlapping GUI/CLI moves, and finished
  journals are pruned while recoverable ones are retained.
- **Monitored during the copy** — if Steam, a game or a Proton/Wine process starts mid-move, the
  operation stops before the manifest commit; close it and resume.
- **Batch moves** — move several games in one queue, pause/resume, cancel safely and keep source
  backups when you want extra assurance.
- **Library Health** — finds stale lock files, orphaned data, duplicate appids and broken
  `compatdata` links without deleting anything.
- **NTFS Doctor** — checks or creates the recommended shared `compatdata` symlink for NTFS libraries.

### Desktop and automation

- **Desktop integration** — desktop notifications, optional Start Steam after a move, sleep
  inhibition, Light/Dark/System themes and quick SteamDB/ProtonDB links.
- **Command line** — the same move engine is available for scripting and diagnostics. See the
  [CLI reference](docs/CLI.md).

## Safety

- Steam must be completely closed while moving a game.
- The destination library must already be registered in Steam. A move updates only the moved game's
  existing Steam metadata, with a backup; it does not add libraries or drive entries.
- Network and unknown filesystem types are refused. FUSE `fuseblk` mounts are ambiguous and carry
  an explicit warning rather than being silently treated as NTFS.
- Full SHA-256 verification is on by default. Quick verification is an explicit opt-out that checks
  paths and sizes only.
- Snap Steam is detected but unsupported.
- Native Linux games on NTFS/exFAT are not officially supported by Steam. Proton games usually work,
  but executable permissions cannot be preserved on those filesystems.

See [docs/SAFETY.md](docs/SAFETY.md) for the transaction order, rollback rules and full safety model.

## Limitations

- This is alpha software. Keep backups, and test the first move with a small game.
- Resume is supported only for operations interrupted **before** the Steam manifest commit. There is
  no one-click undo after a completed move.
- Steam Library Mover does not create or register Steam libraries, adopt unregistered games, or
  modify cloud saves.
- xattrs/ACLs and sparse extents are not preserved. Symlinks, modes and mtimes are preserved where
  the destination supports them.
- A Flatpak build is not currently offered. Snap Steam is detected but not supported for moves.

## Supported Steam installations

| Installation | Support |
| --- | --- |
| Native Steam | Supported |
| Flatpak Steam | Supported when Steam Library Mover runs natively (AppImage, AUR or source build) |
| Snap Steam | Detected but moves are not supported |
| Custom Steam root | Selectable under **Settings → Steam root override** |

If you have multiple Steam installations, choose the correct one in the Steam installation selector.

## NTFS libraries

Steam recommends keeping Proton prefixes on a Linux filesystem. If your games are on NTFS, keep the
shared `compatdata` directory on a Linux filesystem and link it into the NTFS library:

```bash
mkdir -p ~/.local/share/Steam/steamapps/compatdata
ln -s ~/.local/share/Steam/steamapps/compatdata /mnt/games/SteamLibrary/steamapps/compatdata
```

The **NTFS Doctor** can check this setup. It never deletes a non-empty prefix directory. See
[docs/NTFS.md](docs/NTFS.md) for mount options, permissions and troubleshooting.

## Blocked moves

A game may be blocked for a real reason, for example:

- the source or destination library is mounted read-only;
- the destination is NTFS and the game contains filenames that differ only by case;
- the destination does not have enough free space;
- Steam or a Proton/Wine game is still running;
- the game is updating or its payload is missing.

The review dialog names the blocker and offers **Why blocked?** with the relevant paths and
guidance. A blocked move is not attempted.

## Command line

```bash
# List installed games and libraries
steam-library-mover-cli list

# Preview a move; changes nothing
steam-library-mover-cli plan --appid 346010 --to /mnt/games/SteamLibrary

# Move after reviewing the plan
steam-library-mover-cli move --appid 346010 --to /mnt/games/SteamLibrary --yes

# Check library health
steam-library-mover-cli health

# Inspect, resume or roll back an interrupted operation
steam-library-mover-cli recover --list
steam-library-mover-cli recover --resume <operation-id>
steam-library-mover-cli recover --rollback <operation-id>
```

See the [CLI reference](docs/CLI.md) for all commands, options, exit codes and JSON output.
`steam-library-mover-cli --help` also prints the global option list.

## Troubleshooting

| Problem | What to do |
| --- | --- |
| Steam's own mover fails with “content file locked” | Use Steam Library Mover; it handles shared Proton prefixes without moving a prefix onto itself. |
| A library is read-only | Check `findmnt -T <library path>` and remount it read-write with the correct permissions. |
| Move blocked by an NTFS case collision | Use a case-sensitive Linux filesystem, or resolve the conflicting game file with a backup. The app does not modify game files automatically. |
| Game is not listed | Click **Rescan**, check the selected Steam installation, or set a custom Steam root in Settings. |
| Move interrupted or verification failed | Open **Recovery** and resume the operation, or run `steam-library-mover-cli recover --list`. See [Recovery](docs/CLI.md#recover). |
| Need the operation log | Open **Recovery → View log**, or read `~/.local/state/steam-library-mover/operations/<operation-id>/log.txt`. |
| The AUR build still looks old | Check `command -v steam-library-mover`. An old copy in `/usr/local/bin` can shadow `/usr/bin/steam-library-mover`. |
| AppImage will not start | Make it executable and try `APPIMAGE_EXTRACT_AND_RUN=1 ./Steam_Library_Mover-x86_64.AppImage`. |

If a move is blocked and you are unsure why, the **Why blocked?** button in the review dialog
includes the exact paths and guidance. If the problem is not explained there,
[open an issue](https://github.com/NasirA1991/steam-library-mover/issues).

## Documentation

| Document | Use it for |
| --- | --- |
| [Installation](docs/INSTALL.md) | AUR, AppImage, source build, dependencies and uninstall |
| [CLI reference](docs/CLI.md) | Commands, options, exit codes, JSON and recovery |
| [Safety model](docs/SAFETY.md) | Transaction order, journals, rollback and deletion rules |
| [NTFS libraries](docs/NTFS.md) | Proton prefixes, mount options and NTFS Doctor |
| [Architecture](docs/ARCHITECTURE.md) | Code layout, data flow and recovery design |
| [Testing](docs/TESTING.md) | Automated tests and manual acceptance checks |
| [Packaging](docs/PACKAGING.md) | Release builds, AUR, AppImage, Flatpak and supply chain |
| [Roadmap](docs/ROADMAP.md) | Current priorities and deliberate non-goals |
| [Release notes](docs/RELEASE_NOTES.md) | Changes in the current release |
| [Contributing](CONTRIBUTING.md) | Development setup, tests and pull-request expectations |
| [Security policy](SECURITY.md) | Private reporting of destructive or security-sensitive bugs |

## Privacy

Steam Library Mover does not include telemetry or require an account. By default it fetches game
cover art from Valve's public Steam CDN; that request includes the game's appid and your IP address.
Disable artwork in **Settings** or set `SLM_DISABLE_ARTWORK=1` to keep the app fully local.

## Contributing and support

Bug reports and feature requests are welcome on the
[issue tracker](https://github.com/NasirA1991/steam-library-mover/issues). Read
[CONTRIBUTING.md](CONTRIBUTING.md) before sending a pull request. Report security-sensitive issues
privately as described in [SECURITY.md](SECURITY.md).

## License

GPL-3.0-or-later. Not affiliated with Valve Corporation. Steam is a trademark of Valve Corporation.
