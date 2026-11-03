# Roadmap decisions

This is a deliberately safety-first review of the larger improvement roadmap, current to
0.1.0 development builds.

## Adopted now

- **Library Health and stale-lock cleanup.** Read-only scan for stale `.lock` and manifest temp
  files, orphaned payloads/prefixes/shadercache/Workshop data, duplicate appids, broken compatdata
  links and source backups. Quarantine renames safe lock/temp files into hidden
  `.slm-quarantine` folders; it never deletes them. CLI: `steam-library-mover-cli health`.
- **Theme and contrast work.** System/Light/Dark palettes, runtime switching, corrected library
  delegate colours, status symbols, readable secondary text and warning colours. Explicit
  Light/Dark use Fusion for consistent menubar/toolbar chrome; the KDE title bar remains a system
  window-decoration choice.
- **NTFS Doctor clarity.** The Doctor now states explicitly when every NTFS library already has a
  valid compatdata symlink, distinguishes configured/broken/missing states, and treats an existing
  correct target as a no-op instead of trying to recreate the symlink.
- **Reflink copy fast path.** `FICLONE` is attempted for each regular file. On Btrfs/XFS and other
  reflink-capable filesystems the destination is instant and space-efficient; unsupported
  filesystems fall back to the normal byte copy. Verification and journaling are unchanged.
- **UI polish.** Short status badges (with Ready rows left quiet), a populated empty details pane,
  compact rows by default, a draggable table/details splitter, a slim Steam-closed banner, clickable
  column sorting, filter counts, selected-game count, relative last-played dates, custom drive-type
  bars, usage text, destination free-space projections/disabled impossible targets, staged progress,
  explicit checkboxes, a transfer summary strip, a collapsible details bar and a Preflight-only review
  with speed/ETA and cancellation during verification, a transfer-space tooltip, context menu
  actions, minimum window width, full-path tooltips, and less-clipped Doctor/Health tables.
- **Existing batch features retained.** Multi-select queue, drag-and-drop, pause/resume,
  keep-source backups, notifications, sleep inhibition and Start Steam.

## Deliberately deferred

- **Create/register Steam libraries** (roadmap item 7). The user explicitly does not want the
  application adding new libraries or drive entries to Steam's database. This remains a non-goal
  unless that policy changes.
- **Adopt unregistered games.** Creating or synthesising manifests is higher risk than moving a
  game Steam already knows about. Keep it experimental and out of the stable flow.
- **Same-filesystem directory rename.** Atomic whole-directory renames conflict with the existing
  per-component journal/rollback model and can fail across Btrfs subvolumes. The reflink path
  captures much of the benefit without changing rollback semantics.
- **Crash-resume and one-click Undo.** Valuable, but need a per-file resumable journal and reverse
  operation design with crash-injection tests. Do not bolt them onto the current journal.
- **Guided destructive repairs** (delete stale manifest, delete orphan payload/prefix, reclaim
  space). Library Health detects and explains these; automatic deletion needs a preview and a
  stronger journaled operation.
- **Library UUID identity / automatic relocation.** Rewriting library paths in Steam metadata is a
  database mutation and needs careful multi-mount testing.
- **Prefix/launch-option path rewriting.** Detection should come before rewriting. A bad rewrite can
  break saves or wrappers; this deserves its own preflight and test fixtures.
- **Rules/tiering, SMART warnings, scheduled moves, D-Bus API.** Useful later; they add automation
  risk and should stay reviewable, never silent.
- **Flatpak host-process validation.** The Flatpak build remains gated until host process detection
  and Flatpak Steam permissions are tested.

## Non-goals

- Downloading or validating games outside Steam's ownership checks.
- Bypassing Steam's library management or adding unrequested database entries.
- Managing non-Steam launchers in the core mover.
