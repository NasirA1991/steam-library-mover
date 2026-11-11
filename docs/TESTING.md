# Testing

## Automated suite

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure --timeout 60
```

The suite has 15 CTest tests. Run it as a normal user; some filesystem and GUI tests skip when
run as root, and a real move is always refused as root. The tests use temporary Steam roots, fixture
manifests and injectable mount tables. They do not need a writable real Steam library.

| Test | Focus |
| --- | --- |
| `tst_vdf` | Valve KeyValues parsing, escapes, comments, duplicate keys, round-trips and edits. |
| `tst_appmanifest` | ACF parsing, appid/install-dir checks and state flags. |
| `tst_mountinfo` | `/proc/self/mountinfo` parsing, path escapes and longest-prefix matching. |
| `tst_filetree` | No-follow scanning, hardlinks, symlinks, Windows names, case collisions and FAT32 limits. |
| `tst_fileops` | Descriptor-relative recursive deletion, symlink-swap resistance and file roots. |
| `tst_instance_lock` | Machine-wide lock rejection and reuse after release. |
| `tst_library` | `libraryfolders.vdf` loading, compatdata symlink resolution and app-map updates. |
| `tst_mover` | End-to-end moves, prefixes, resume/repair, rollback, copy-time hash verification, parallel/sparse copies, atomic finalization, pause/cancel, NTFS, keep-source, batch VDF and pruned journals. |
| `tst_doctor` | Compatdata states, safe symlink setup, non-empty-directory refusal and idempotent targets. |
| `tst_scancache` | Scan-cache round-trip and isolated cache directories. |
| `tst_health` | Stale lock/temp/orphan detection and quarantine renames. |
| `tst_theme` | Light/dark palette contrast. |
| `tst_uiformat` | Drive names, home display, subtitles and full-path tooltips. |
| `tst_artwork` | Local artwork, cache hits and disabled downloads. Live CDN fetching is a manual, network-dependent check. |
| `tst_gui` | Offscreen scan, model, selection, review selection/refresh, batch queue, health dialog and transfer strip. |

The GUI tests set `QT_QPA_PLATFORM=offscreen` and use temporary `SLM_STATE_DIR`, `SLM_CACHE_DIR`
and `XDG_CONFIG_HOME` paths. Process detection is injected in tests so a running host Steam client does
not make the suite flaky.

CI runs the GCC Debug and Clang Release suites on Ubuntu 24.04, plus an Arch Linux release build.
Local results can differ from CI if the host has a different Qt patch release or a running Steam
client; see the sandbox caveat below.

## Manual NTFS test

`tests/manual/ntfs-loop.sh` creates two loopback NTFS images, mounts them with ntfs-3g, builds
library fixtures and runs a move. It requires root and is intended for a developer machine, not CI.
Always unmount the images and clean up loop devices afterwards.

## Real-machine acceptance

Never test the first real move on a large game.

1. Back up `~/.local/share/Steam/steamapps/libraryfolders.vdf` and the game's manifest.
2. Pick a small game that has a Proton prefix and is not updating.
3. Move it from one NTFS library to another. Confirm:
   - the source folder and manifest are gone;
   - the destination has the game and manifest;
   - the shared prefix remains where it was;
   - Steam shows the game only in the destination;
   - the game launches and saves are present.
4. Move it back.
5. Test a game from NTFS to ext4/btrfs, including prefix migration.
6. Test pause, cancel and recovery before trusting a very large game.
7. Test a batch of two games and confirm the second move sees the updated library database.

## Test environment caveat

The development sandbox may expose host filesystems read-only and may have Steam running. The suite
is written to avoid both: it uses temporary directories and an injectable mount table, and it does
not write to the real Steam libraries. Do not "fix" a test by making it depend on the host's real
Steam installation.

## Missing coverage

- Privileged loopback NTFS integration in CI.
- Crash injection at every executor phase; rollback and resume paths exist, but not every
  power-loss window is simulated.
- Full GUI keyboard/accessibility interaction tests.
- Performance and free-space behavior during very large copies.
- Flatpak host-process detection and shutdown in a real sandbox.

## See also

- [Contributing](../CONTRIBUTING.md)
- [Architecture](ARCHITECTURE.md)
- [Safety model](SAFETY.md)
- [Installation](INSTALL.md)
