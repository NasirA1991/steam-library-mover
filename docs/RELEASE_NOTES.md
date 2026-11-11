# Steam Library Mover v0.1.8 — public alpha

**This is an alpha release of a tool that moves real game files. Read the safety notes and keep an
independent backup of irreplaceable data.**

This release closes the first batch of review findings. The most important fix prevents the NTFS
acknowledgement from being bypassed, and the executor now reloads disk state immediately before
the manifest commit.

## Highlights

- Qt 6 desktop app plus a headless CLI.
- Journaled, verified moves between Steam libraries.
- Shared `compatdata` handling for NTFS libraries.
- Checks for pending updates, running Steam/Proton/Wine, invalid Windows names, NTFS case
  collisions, insufficient space, read-only libraries and duplicate appids.
- Review dialog with visible blockers, full case-collision paths and a **Why blocked?** explanation.
- Batch queue, drag and drop, pause/resume, keep-source backups, reflink copy fast path.
- Library Health with non-destructive quarantine, NTFS Doctor and Recovery.
- Light/dark themes, compact game list, explicit checkboxes and a transfer summary strip.

## What changed in 0.1.8

- NTFS acknowledgement is enforced by the core planner. Reopening the review dialog no longer
  hides an unacknowledged Windows filesystem.
- Mounts are reloaded and revalidated at execute time and immediately before the manifest commit.
- Recovery offers **View log**, **Open folder** and **Forget** actions.
- Pause/resume speed and ETA exclude time spent paused.
- VDF parsing rejects unterminated quotes; unsupported file types are preflighted; volatile
  destination filesystems produce a warning.
- Runtime detection only treats the exact `proton` name as a system component, and VDF app sizes
  use the scanned game component.
- Artwork downloads are capped and content-validated; Steam launch uses known installation paths
  before `PATH`.

## Installation

Download one of the release assets:

- `Steam_Library_Mover-x86_64.AppImage` — portable artifact.
- `steam-library-mover-*.pkg.tar.zst` — Arch Linux / CachyOS package.
- `steam-library-mover-*.tar.gz` — source archive.

The AUR recipe is in `packaging/aur`. A Flatpak build is **not** part of this release because
host Steam/Proton process detection inside the sandbox has not been validated.

## Known limitations

- Public alpha: test on a small game and keep backups.
- Destination libraries must already be registered in Steam.
- Crash-resume is not implemented; rollback and keep-source backups cover recovery.
- Native Linux games on NTFS are not officially supported by Steam.
- Case-only files and symlinks cannot be represented on case-insensitive filesystems. The app
  blocks these destinations instead of silently changing game data.
- Snap Steam is detected but unsupported.
- Flatpak packaging and host-process detection remain gated.

## Safety

Steam must be closed. A move verifies the destination and syncs it before updating the Steam
manifest; source cleanup starts only after the manifest commit. If an operation fails before that
point, the source remains usable.

Report security issues privately as described in `SECURITY.md`.
