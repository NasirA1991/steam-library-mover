# Architecture

## Layering

- `src/core/` — Qt Core only. All filesystem, VDF, planning and transaction logic. No widgets.
- `src/cli/` — headless frontend using the same core.
- `src/gui/` — Qt Widgets frontend and worker threads. It does not implement its own file-moving
  logic.

This keeps the dangerous code testable without a display and prevents the GUI and CLI from inventing
different move sequences. The core is the only layer that copies, deletes or rewrites Steam metadata.

## Main components

| Component | Responsibility |
| --- | --- |
| `types` | Shared data model: libraries, games, plans, components, issues and progress. |
| `vdf` | Valve KeyValues parser/serializer that preserves unknown fields. |
| `appmanifest` | Parse and validate `appmanifest_<appid>.acf`, state flags and install directory. |
| `mountinfo` | Parse `/proc/self/mountinfo`, classify filesystems and match longest mount prefix. |
| `scanner` | Detect Steam roots, load libraries, scan manifests and classify games/components. |
| `scancache` | Display-only startup snapshot; never used by the planner or executor. |
| `library` | `libraryfolders.vdf` loading, compatdata inspection and atomic app-map edits. |
| `filetree` | Recursive no-follow scan, sizes, hardlinks, Windows filename/case validation. |
| `fileops` | Descriptor-relative, no-follow recursive deletion within one filesystem. |
| `copyengine` | POSIX copy, sparse and bounded-parallel file copies, copy-time source hashes, atomic rename, progress, cancellation, reflink and verification. |
| `planner` | Build a move plan, validate blockers and compute free space per filesystem. |
| `executor` | Journaled transaction, resume, backups, sync, manifest switch, VDF update and cleanup. |
| `journal` | State/log/backup directories, atomic JSON writes, retention and operation listing. |
| `instance_lock` | Machine-wide advisory lock shared by GUI and CLI. |
| `process_check` | Detect host Steam/Proton/Wine processes and prefix users. |
| `doctor` | NTFS findings and safe compatdata symlink setup. |
| `health` | Read-only stale-state scan and same-filesystem `.slm-quarantine` renames. |
| GUI artwork provider | Local Steam `librarycache` first, then optional cached CDN box art. |
| GUI uiformat | Drive-name labels, compact subtitles and full-path tooltips. |
| GUI review dialog | Per-game selection, blockers, NTFS acknowledgement, fresh preflight and accepted-plan options. |
| GUI batch queue | Cumulative space reservations, sequential execution and VDF refresh. |
| GUI metadata | Compat tool, last played/build ID and Proton/Native detection. |
| GUI integrations | Sleep inhibitor, desktop notifications and Start Steam. |
| GUI themes | System palette, explicit Light/Dark palettes, theme persisted in QSettings. |

## Data flow

```text
Steam roots + /proc/self/mountinfo
        │
        ▼
 scanSteam() ──► SteamScan {libraries, games, issues}
        │
        ▼
MovePlanner::plan(scan, appId, destinationLibrary, options, mounts,
                  &error, reservedBytes)
        │
        ▼
 MovePlan {components, issues, space, snapshot}
        │
        ▼
MoveExecutor::execute(plan, mounts, progress, cancel, pause, resumeJournal)
        │
        ├─ journal + log + backups
        ├─ copy game, prefix, shader, Workshop components
        ├─ verify each component, then syncfs
        ├─ destination manifest (atomic)
        ├─ libraryfolders.vdf (atomic, backup)
        ├─ remove source manifest
        └─ source payload cleanup
```

The GUI scans and plans on `QThread` workers and shows the result for explicit review. Batch planning
reuses a planner thread per game with a cumulative reservation map. Execution uses one `MoveThread`
at a time; pause is passed into the copy engine, and the VDF snapshot is refreshed between games
because each move updates the library database.

The Steam banner polls process state asynchronously so the window does not block. Review and quit
actions use the cached result, while the planner and executor always re-check processes on their
worker threads. Recovery resume is currently run synchronously from the Recovery dialog with a modal
progress dialog; normal scan, plan and move work does not block the GUI event loop.

A small `ArtworkProvider` fills missing box art asynchronously from Steam's public static CDN after
checking the local Steam `librarycache`, and stores it under the XDG cache directory. The UI displays
drive names with full paths only in tooltips; `uiformat` derives a volume label when available and
otherwise a readable name from the mount path.

## Concurrency and process safety

- `InstanceLock` takes an exclusive `flock` on `/run/lock/steam-library-mover.lock`, falling back to
  `/tmp/steam-library-mover.lock` when the system lock directory is not writable. A second GUI or CLI
  instance is refused while the lock is held.
- Planning checks for Steam, the game and Proton/Wine prefix users. Execution rechecks at start, and
  the executor polls every 30 seconds during copy and verification. A process that appears mid-move
  stops the operation before the manifest commit so it can be resumed after the process exits.
- Mount state and free space are revalidated at execute start and immediately before commit. Resume
  additionally compares the current `libraryfolders.vdf` and game manifest with the operation's
  backups.

## VDF handling

The parser stores an ordered tree of keys and string values. It supports quoted and unquoted tokens,
escapes, comments, a UTF-8 BOM and duplicate keys. Unknown fields survive a round trip. The editor
changes only the destination/source `apps` maps; saving uses `QSaveFile` (temporary file plus rename)
and then fsyncs the parent directory. A backup is stored in the operation directory before any edit.

## Copy protocol

- Directories are created first, parent-first.
- Each regular file is copied to a uniquely named `.slm-part-*` temporary file in the destination
  directory and then moved into place with `rename(2)`. A crash cannot leave a complete-looking
  partial file, and replacing an existing file does not expose a missing-destination window.
- Symlinks are recreated as symlinks and never followed. Modes and mtimes are preserved best-effort;
  symlinks and hard links also use a temporary name plus atomic rename.
- Hardlinks inside the moved tree are recreated with `link()`. Hardlink groups are copied
  sequentially; independent files may use up to four copy workers on larger transfers.
- Sparse files are copied by data extent using `SEEK_HOLE`/`SEEK_DATA` when supported, with a normal
  copy fallback.
- Normal and sparse copies compute a source SHA-256 while copying. Full verification reuses it only
  after checking source size, timestamps, inode and ctime; otherwise it hashes both sides. Reflinks,
  skipped files and hardlink duplicates have no copy-time hash and always fall back.
- Verification compares types, path sets, sizes and symlink targets. Full mode hashes every file;
  up to four verification workers hash independent files concurrently, with serialized, monotonic
  progress.
- `syncfs()` runs on every affected destination filesystem before the manifest switch.

Deletion of source payloads is anchored at directory descriptors, opens children with `O_NOFOLLOW`,
refuses to cross a filesystem boundary and never follows symlinks. A failed cleanup leaves the
operation in `done-with-warnings` rather than deleting in place.

## State and recovery

State lives under `$XDG_STATE_HOME/steam-library-mover` (normally
`~/.local/state/steam-library-mover`). The test suite overrides it with `SLM_STATE_DIR`.

An interrupted operation appears in Recovery. Before the manifest commit starts, it can be resumed:
verified destination components are reused, while incomplete or corrupt components are removed and
copied again. The original VDF and appmanifest backups are compared with the current files first, so
a changed library database or game build cannot be resumed silently. After the manifest commit
starts, resume is refused; rollback is allowed only while the source manifest still exists. Rollback
removes destination components recorded in the journal, restores the VDF backup and deletes the
destination manifest. Source data is never touched by rollback.

Finished journals are pruned to the 20 most recent entries; unfinished, failed and cancelled
operations are retained until Recovery resumes, rolls back or forgets them.

## Why not use Steam's content API?

The tool deliberately does not invoke Steam's mover. Steam's mover has the shared-compatdata bug and
its content-lock/database interactions are opaque. The on-disk format is simple and stable enough to
handle directly, with backups and verification. The tool does not read Steam credentials or modify
user config or cloud saves.

## See also

- [Safety model](SAFETY.md)
- [CLI reference](CLI.md)
- [Testing](TESTING.md)
- [Installation](INSTALL.md)
