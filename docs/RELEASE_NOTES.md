# Steam Library Mover v0.1.9 — public alpha

**This is an alpha release of a tool that moves real game files. Read the safety notes and keep an
independent backup of irreplaceable data.**

This release completes the P1 safety pass from the v0.1.7 review: interrupted moves can be resumed
or repaired, deletion no longer follows symlinks, overlapping instances are blocked, process changes
during a copy stop the move before commit, and the GUI no longer freezes while checking Steam.

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

## What changed in 0.1.9

- **Resume and repair** — before the manifest commit, Recovery can reuse verified destination data,
  remove incomplete/corrupt components and copy them again. A failed verification gets one
  automatic remove-and-recopy attempt. Changed source manifests or library databases are refused.
- **Race-safe cleanup** — recursive deletion is descriptor-relative, refuses to follow symlinks
  swapped in during deletion, and never crosses a filesystem boundary.
- **One writer at a time** — a machine-wide advisory lock covers the GUI and CLI, including
  different users. A 30-second process monitor stops the copy if Steam, a game or a Proton/Wine
  process starts; the operation can then be resumed.
- **Responsive GUI** — Steam polling and the graceful shutdown request run off the main thread.
  NTFS Doctor also refuses to edit a library while Steam is running.
- **Bounded recovery history** — finished journals are pruned to the 20 most recent. Unfinished,
  failed and cancelled operations remain until recovered or forgotten.
- **Pinned release supply chain** — GitHub Actions are pinned to commit SHAs; AppImage tooling is
  pinned to tagged releases and verified SHA-256 sums; release assets carry GitHub build-provenance
  attestations.

## Installation

Download one of the release assets:

- `Steam_Library_Mover-x86_64.AppImage` — portable artifact.
- `steam-library-mover-*.pkg.tar.zst` — Arch Linux / CachyOS package.
- `steam-library-mover-*.tar.gz` — source archive.

The AUR package is `steam-library-mover`. A Flatpak build is **not** part of this release because
host Steam/Proton process detection inside the sandbox has not been validated.

Verify downloads with `SHA256SUMS`, then optionally check provenance:

```bash
gh attestation verify Steam_Library_Mover-x86_64.AppImage \
  --repo NasirA1991/steam-library-mover
```

## Known limitations

- Public alpha: test on a small game and keep backups.
- Destination libraries must already be registered in Steam.
- Resume is offered only before the manifest commit. Once Steam metadata has been updated, use the
  normal Recovery rollback rules; automatic resume is intentionally refused.
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
