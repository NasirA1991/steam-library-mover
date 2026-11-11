# Steam Library Mover v0.2.0 — public alpha

**This is an alpha release of a tool that moves real game files. Read the safety notes and keep an
independent backup of irreplaceable data.**

This release makes verified transfers faster without weakening the copy → verify → commit →
cleanup contract. It also gives the batch review per-game control and makes the main window adapt to
smaller displays.

## Highlights

- Qt 6 desktop app plus a headless CLI.
- Journaled, verified moves between Steam libraries.
- Shared `compatdata` handling for NTFS libraries.
- Checks for pending updates, running Steam/Proton/Wine, invalid Windows names, NTFS case
  collisions, insufficient space, read-only libraries and duplicate appids.
- Batch review with per-game selection, fresh preflight checks and **Why blocked?** explanations.
- Batch queue, drag and drop, pause/resume, keep-source backups, reflink copy fast path.
- Library Health with non-destructive quarantine, NTFS Doctor and Recovery.
- Light/dark themes, compact game list, persistent window layout and a transfer summary strip.

## What changed in 0.2.0

### Faster verified copies

- **Copy-time source hashes** — normal and sparse copies compute the source SHA-256 while copying.
  Full verification can then hash only the destination and reuse that source hash.
- **Metadata-checked reuse** — a cached source hash is trusted only when the source size,
  timestamps, inode and ctime still match. Reflinks, skipped files, hard-link duplicates and changed
  sources fall back to hashing both sides.
- **Bounded parallelism** — independent regular files are copied by up to four workers on larger
  transfers, and verification hashes up to four files concurrently. Progress stays monotonic and
  cancellation stops the workers.
- **Sparse files stay sparse** — copies preserve holes with `SEEK_HOLE`/`SEEK_DATA` where the
  destination filesystem supports it.
- **Atomic file completion** — files, symlinks and hard links are moved into place with `rename(2)`
  instead of unlinking the destination first.
- **I/O advice** — sequential reads/writes use `posix_fadvise`, and copied source pages are dropped
  from the page cache after a non-reflink copy.

### Review and window

- **Per-game selection** — uncheck games in the batch review. Payload totals, destination-space
  figures, blockers, the NTFS acknowledgement and Start enablement all follow the current selection.
- **Fresh preflight** — **Preflight only** reloads mounts and revalidates the selected plans rather
  than repeating a stale review.
- **Verification progress** — full verification reports hashed bytes and a determinate progress bar
  instead of staying indeterminate.
- **Layout memory** — window geometry, splitter positions, table header layout, sort order and
  sidebar visibility are remembered.
- **Smaller displays** — the minimum window size is 900×560, and the library sidebar can be hidden
  from **View → Library sidebar** (`Ctrl+B`).

## Safety notes

- Full SHA-256 verification remains the default. Quick verification checks paths and sizes only and
  cannot detect same-size corruption.
- Destination-only verification trusts the copy-time source hash only after rechecking source
  metadata. It does not protect against a source changed while preserving all metadata.
- The source remains authoritative until the destination verifies and the Steam manifest is
  committed. Cancelling before that point keeps the source intact.
- This is an alpha release: keep an independent backup of saves and irreplaceable data.

## Installation

Download one of the release assets:

- `Steam_Library_Mover-x86_64.AppImage` — portable artifact.
- `steam-library-mover-*.pkg.tar.zst` — Arch Linux / CachyOS package.
- `steam-library-mover-*.tar.gz` — source archive.

The AUR package is `steam-library-mover`. A Flatpak build is **not** part of this release because
host Steam/Proton process detection inside the sandbox has not been validated.

## Known limitations

- Resume is supported before the manifest commit, not as an undo of a completed move.
- xattrs, ACLs and file capabilities are not preserved.
- Flatpak as an application format remains experimental and is not published.
