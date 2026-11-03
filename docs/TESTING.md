# Testing

## Automated

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

The suite covers:

- VDF parsing, escaping, duplicate keys, round-trips and edits.
- ACF parsing, appid/filename mismatch and state-flag handling.
- Mountinfo parsing, path escapes and longest-prefix matching.
- File-tree scanning, hardlinks, symlinks, Windows filename rules, case collisions and FAT32 limits.
- Library loading, compatdata symlink resolution and `libraryfolders.vdf` app movement.
- End-to-end shared-prefix move: game + manifest + shader + Workshop; source removal; destination
  verification; VDF update; shared prefix untouched.
- NTFS preflight blocking for invalid names and acknowledgement gating.
- Offscreen GUI smoke test: startup scan, fixture game model, drive-name column and tooltip, selection,
  review-button enablement, screenshot.
- UI formatting tests for drive names, home library display, subtitles and full-path tooltips.
- Box-art provider tests for local Steam artwork, cache hits and disabled downloads; live CDN fetching
  is a manual check (network-dependent).
- Pause/resume copy test, keep-source-backup test, compat-tool metadata test and a sequential
  two-game move test that refreshes the VDF snapshot between games.
- Automated offscreen batch-move GUI flow: select two fixture games, accept the review dialog,
  wait for the queue summary and verify both games were registered at the destination.
- Library Health tests for stale lock/temp/orphan detection and quarantine renames; GUI test opens
  the Library Health dialog offscreen.
- Theme palette tests for light/dark contrast.
- Scan-cache round-trip tests and isolated `SLM_CACHE_DIR` use in GUI tests.
- GUI coverage for checkbox selection, the transfer strip, the collapsible details bar, a scrollable review dialog and the redesigned review flow.
- Doctor no-op test for an already-correct compatdata target; drive-type/usage tooltip coverage;
  empty-details, blank-Ready, destination-placeholder, filter-count and payload-size assertions in
  the GUI test; the batch flow exercises the staged progress dialog. Verification cancellation is
  covered directly in the mover test.

## Manual NTFS test

`tests/manual/ntfs-loop.sh` creates two loopback NTFS images, mounts them with ntfs-3g, builds
library fixtures and runs a move. It requires root and is intended for a developer machine, not CI by
default. Always unmount/clean up the loop devices afterwards.

## Real-machine acceptance

Never test the first real move on a large game.

1. Back up `~/.local/share/Steam/steamapps/libraryfolders.vdf` and the game's manifest.
2. Pick a small game that has a Proton prefix and is not updating.
3. Move it from one NTFS library to another. Confirm:
   - source folder/manifest are gone;
   - destination has the game and manifest;
   - the shared prefix remains where it was;
   - Steam shows the game only in the destination;
   - the game launches and saves are present.
4. Move it back.
5. Test a game from NTFS to ext4/btrfs (prefix migration).
6. Test cancel and recovery before trusting a very large game.

## Current test environment caveat

The development sandbox may expose host filesystems read-only. Tests use temporary directories and
an injectable mount table so they do not need write access to the real Steam libraries.

## Missing coverage / TODO

- Privileged loopback NTFS integration in CI.
- Crash injection at every executor phase (rollback tests exist for recovery paths, but not yet all
  power-loss windows).
- Full GUI interaction tests with keyboard/accessibility checks.
- Performance/space checks during a copy.
- Flatpak host-process detection test.
