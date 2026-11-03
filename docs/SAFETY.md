# Safety model

Steam Library Mover is a destructive file-management tool. Its safety claims depend on the checks
and ordering described here; do not weaken them casually.

## Requirements before a move

- Steam is completely closed, including `steamwebhelper`, Flatpak Steam, and any Proton/Wine
  processes.
- The game is fully installed, not updating, validating, downloading or otherwise busy.
- The destination is an existing Steam library registered in `libraryfolders.vdf`.
- The user has write access to both libraries and enough free space.
- No destination game, manifest, shader cache, Workshop data, or **different** Proton prefix exists.
- The destination filesystem can represent every filename (NTFS/exFAT/FAT32 checks run first).
- The tool is not running as root.

The planner records these as blockers and the executor rechecks the important ones immediately
before modifying anything.

## Move transaction

```
Planned
  → copy game + prefix + shader/Workshop payload
  → verify each component
  → sync destination filesystems
  → revalidate source manifest + library database + processes
  → write destination manifest atomically
  → update libraryfolders.vdf atomically (backup first)
  → remove source manifest
  → move source payload to a trash name and delete it
  → done
```

**Source deletion only happens after the destination has been verified and synchronized.** The
source manifest is removed only after the destination manifest is written and the library database
is updated. Until that point the source installation remains valid.

If the source manifest cannot be removed, the tool restores the original `libraryfolders.vdf` and
removes the destination manifest, leaving the source authoritative.

If cleanup fails after the source manifest is removed, the game is already registered and complete
at the destination. The operation is marked `done-with-warnings`; remaining source files can be
deleted manually after checking the destination.

## Prefix rule

Resolve the effective prefix path on both sides:

- If the library has a real `compatdata` directory, the prefix is `<compatdata>/<appid>`.
- If it is a symlink, resolve the symlink first.

If the source and destination effective prefixes resolve to the same directory, **skip the prefix
move**. This is the primary fix for Steam's “content file locked” bug with shared compatdata
symlinks. If both prefixes exist and resolve differently, the move is blocked; the tool never merges
or overwrites Proton prefixes.

## Backups and journal

Each operation writes to `~/.local/state/steam-library-mover/operations/<opId>/`:

- `journal.json` — phase, components, hashes, paths, state.
- `log.txt` — human-readable operation log.
- `backup/libraryfolders.vdf` — original library database.
- `backup/appmanifest_<appid>.acf` — original manifest.

Journal and log files are mode 0600. They contain local paths but no Steam credentials or
environment data.

Rollback removes destination components created by the operation, restores the VDF backup, and
removes the destination manifest. It does **not** delete source data. Rollback is refused after the
source manifest has been removed.

## Keep-source backup mode

When `keepSourceBackup` is enabled, the game is registered at the destination exactly like a normal
move, but the source game folder, shader/Workshop data and any moved Proton prefix are renamed to
hidden `.slm-backup-<appid>-<random>` entries instead of being deleted. The source manifest is still
removed so Steam does not see a duplicate installation. Source disk space is therefore not freed
until you remove those backups. Paths are reported in the completion dialog and logged in the
journal.

## Library Health quarantine

**Tools → Library Health…** can move stale lock and manifest temporary files into a hidden
`.slm-quarantine` directory next to the original file. The original file is renamed, not deleted, so
it can be restored. Quarantine is refused while Steam or any Proton/Wine game is running. Source
backups created by keep-source mode are reported but never removed automatically.

The health scan is read-only. Orphaned payloads, prefixes, shadercache and Workshop content are
reported for manual investigation; the app does not delete them.

## Startup scan cache

The startup cache is a display convenience only. It is written after a successful scan under
`~/.cache/steam-library-mover/` and may contain stale game/library data. The app always runs a fresh
scan in the background; Review, Library Health and NTFS Doctor are blocked while the cached view is
in use. The cache is never consulted by the planner or executor.

## Reflink copies

On reflink-capable filesystems the copy engine clones each regular file with `FICLONE` and then
performs the same metadata, journal and verification steps as a byte copy. The destination is an
independent CoW file; source changes after the clone do not affect it. If cloning is unsupported,
the engine falls back to the normal read/write loop. The fast path never bypasses verification or
the manifest/VDF commit barrier.

## Pause and cancellation

Pause is checked between copy chunks and at file boundaries; it applies to the current game and to
the start of the next game in a batch queue. Cancellation is checked during copying and verification.
A cancelled move leaves the source intact and may leave a partial destination. Use Recovery to remove
the partial copy. Cancellation is intentionally ignored once the manifest commit begins so that the
library database and manifests cannot be left in a half-updated state.

In a batch queue, each successful move refreshes the VDF snapshot before the next game so that the
next manifest/VDF commit sees the updated library database. Free-space planning reserves the full
batch requirement (including the safety margin) before queueing.

## Deletion safety

- Only known components from the plan/journal are removed.
- Top-level game directories must be real directories, not symlinks.
- Symlinks are unlinked, never followed.
- Source payloads are renamed to a hidden trash name and removed recursively without traversal
  through symlinks.
- The compatdata library symlink itself is never removed.

## Known limitations

- Resume-after-crash is not fully implemented; rollback is supported before the source manifest is
  removed, otherwise Recovery reports what remains. Keep-source backups are a manual fallback.
- Batch queues stop at the first failed move; unfinished games are not attempted. Re-plan them.
- The GUI never creates/registers a new Steam library or adds a new drive entry. It only updates the
  moved game's existing `apps` entry, with a VDF backup.
- Free space is checked at plan time, not continuously during a copy.
- xattrs/ACLs and sparse extents are not preserved; symlinks, modes and mtimes are.
- Flatpak builds cannot detect host Steam/Wine processes without a host-spawn permission; do not
  ship a Flatpak build without validating that path.
- A network filesystem destination is refused; local block-backed filesystems only.
