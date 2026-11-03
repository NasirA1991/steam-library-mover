# Architecture

## Layering

- `src/core/` — Qt Core only. All filesystem, VDF, planning and transaction logic. No widgets.
- `src/cli/` — headless frontend using the same core.
- `src/gui/` — Qt Widgets frontend with worker threads. No file-moving logic of its own.

This keeps the dangerous code testable without a display and prevents the GUI from inventing a
different move sequence.

## Main components

| Component | Responsibility |
|---|---|
| `types` | Shared data model: libraries, games, plans, components, issues, progress |
| `vdf` | Independent Valve KeyValues parser/serializer; preserves unknown fields |
| `appmanifest` | Parse and validate `appmanifest_<appid>.acf`, state flags and install dir |
| `mountinfo` | Parse `/proc/self/mountinfo`, classify filesystems, longest-mount matching |
| `scanner` | Detect Steam roots, load libraries, scan manifests, classify games/components |
| `library` | `libraryfolders.vdf` loading, compatdata inspection, atomic app-map edits |
| `filetree` | Recursive no-follow scan, sizes, hardlinks, Windows filename/case validation |
| `copyengine` | POSIX copy with per-file temp+rename, progress, cancellation, verification |
| `planner` | Build a move plan, validate every blocker, compute space per filesystem |
| `executor` | Journaled transaction, backups, sync, manifest switch, VDF update, cleanup |
| `journal` | State/log/backup directories, atomic JSON writes, operation listing |
| `process_check` | Detect host Steam/Proton/Wine processes and prefix users |
| `doctor` | NTFS findings and safe compatdata symlink setup |
| GUI artwork provider | Local Steam librarycache first, then optional cached CDN box art |
| GUI uiformat | Drive-name labels, compact subtitles and full-path tooltips |
| GUI batch queue | Plans games with cumulative space reservations, executes sequentially and refreshes VDF snapshots |
| GUI metadata | Compat-tool mapping, last played/build ID, heuristic Proton/Native runtime detection |
| GUI integrations | Sleep inhibitor (login1/screensaver), desktop notifications, Start Steam action |
| GUI themes | System palette, explicit Light/Dark palettes, theme persisted in QSettings |
| Core library health | Read-only stale-state scan and same-filesystem `.slm-quarantine` renames |
| GUI scan cache | Per-root JSON display snapshot; background scan remains authoritative |
| Copy engine | Optional `FICLONE` reflink fast path with byte-copy fallback, same journal/verify flow |

## Data flow

```
Steam roots + /proc/self/mountinfo
        │
        ▼
   scanSteam() ──► SteamScan {libraries, games, issues}
        │
        ▼
MovePlanner::plan(scan, appid, destination, options, mounts)
        │
        ▼
   MovePlan {components, issues, space, snapshot}
        │
        ▼
MoveExecutor::execute(plan, mounts)
        │
        ├─ journal + log + backups
        ├─ CopyEngine (game, prefix, shader, Workshop)
        ├─ verify + syncfs
        ├─ destination manifest (atomic)
        ├─ libraryfolders.vdf (atomic, backup)
        ├─ remove source manifest
        └─ source payload cleanup
```

The GUI scans and plans on `QThread` subclasses and shows the result for explicit review. Batch
planning reuses one planner thread per game with a cumulative reservation map. Execution is one
`MoveThread` at a time; pause is passed into the copy engine, and VDF snapshots are refreshed
between games because each move updates the library database.
A small `ArtworkProvider` fills missing box art asynchronously from Steam's public static CDN
(after checking the local Steam `librarycache`), and stores it under the XDG cache directory.
The UI displays drive names with full paths only in tooltips; `uiformat` derives a volume label
when available and otherwise a readable name from the mount path. No
filesystem mutation happens on the GUI thread.

## VDF handling

The parser stores an ordered tree of keys and string values. It supports quoted/unquoted tokens,
escapes, comments, BOM and duplicate keys. Unknown fields survive a round trip. The editor only
changes the destination/source `apps` maps; save uses `QSaveFile` (temp + rename) and then fsyncs the
parent directory. A backup is stored in the operation directory before any edit.

## Copy protocol

- Directories are created first, parent-first.
- Each regular file is copied into a `.slm-part-*` temporary file in the destination directory, then
  renamed into place. A crash cannot leave a complete-looking partial file.
- Symlinks are recreated as symlinks (never followed); modes and mtimes are preserved best-effort.
- Hardlinks inside the moved tree are recreated with `link()`.
- Verification compares types, path sets, sizes and symlink targets; full mode hashes every file.
- `syncfs()` runs on every affected destination filesystem before the manifest switch.

## State and recovery

State lives under `$XDG_STATE_HOME/steam-library-mover` (normally
`~/.local/state/steam-library-mover`). The test suite overrides it with `SLM_STATE_DIR`.

An interrupted operation appears in Recovery. Rollback is allowed only while the source manifest
still exists. It removes destination components recorded in the journal, restores the VDF backup and
deletes the destination manifest. Source data is never touched by rollback.

## Why not use Steam's content API?

The tool deliberately does not invoke Steam's mover. Steam's mover has the shared-compatdata bug and
its content-lock/database interactions are opaque. The on-disk format is simple and stable enough to
handle directly, with backups and verification. The tool does not read Steam credentials or modify
user config/cloud saves.
