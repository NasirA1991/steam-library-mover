# Changelog

## 0.1.0 — 2026-11-03

Initial development snapshot.

- Qt 6 desktop UI and shared-core CLI.
- Drive-name library display with full-path tooltips and larger game-row box art.
- Box art read from Steam's local librarycache, with optional cached CDN fallback.
- Multi-select batch move queue with drag-and-drop, pause/resume and per-game/queue progress.
- Storage usage bars, batch free-space reservations and review-time projections.
- Proton/Native/Auto detection from Steam's compat-tool mapping and local game files; compat-tool
  version, last played, build ID and game/prefix/shadercache/Workshop size breakdown.
- Ready/Blocked/Warnings/Proton/Native/Auto filters and name/size/recent-play/runtime sorting.
- Keep-source-backup mode, sleep inhibition, desktop notifications and Start Steam action.
- System/Light/Dark themes with corrected contrast for dark palettes.
- Library Health scan with stale lock/manifest-temp quarantine, orphan/prefix/shadercache/Workshop
  checks and source-backup listing; also exposed as `health` in the CLI.
- Flatpak Steam root candidates and explicit native/Flatpak/Snap support documentation.
- Light/Dark themes now use palette-driven Fusion chrome so Breeze Dark cannot leave the menu bar
  and toolbar dark. The KDE title bar remains the system window decoration.
- Reflink (`FICLONE`) fast copy with automatic fallback; verified-copy semantics unchanged.
- NTFS Doctor now distinguishes configured/broken/missing compatdata links and no-ops when the
  selected target is already correct.
- Compact rows (default), splitter between table and details, slim Steam-closed banner, clickable
  sortable headers, filter counts, relative last-played dates, drive-type icons, usage text,
  destination free-space projection/disabled targets, payload-only size labeling and `unknown`
  display for missing manifest sizes.
- MainWindow now waits for scan/plan/move/size workers in its destructor, preventing a QThread abort
  when the application exits without a closeEvent.
- Added explicit per-game checkboxes, a persistent selected count and a visible transfer summary
  strip with source, destination and post-move free-space projections.
- Added a collapsible details bar, coloured drive-type bars in the library sidebar, and a redesigned
  review dialog with source/destination cards, temporary-space accounting, method summary,
  Preflight-only action and clearer safety copy.
- Redesigned the progress dialog with a vertical five-stage list, queue-wide progress, speed/elapsed/
  remaining metrics, current file and source/destination context.
- Fixed theme-dependent dark-on-dark review/progress/details text: muted labels now use an explicit
  theme-aware contrast color instead of `palette(mid)`. The transfer review body scrolls on short
  screens, keeps its footer buttons visible, and sizes the game table to its rows instead of leaving
  a large empty block.
- The review now shows a prominent "Move blocked" banner with exact blocker details, full paths for
  NTFS case collisions, an NTFS acknowledgement notice and a "Why blocked?" dialog with guidance.
  Case collisions are prioritised over generic read-only findings in the summary.
- Staged move progress with copy/verify/update/cleanup indicators, speed, elapsed time and ETA;
  selected-game count and a detailed transfer-space summary tooltip.
- Verification now reports an honest busy state instead of resetting to 0%, disables Pause and honors
  Cancel; cancellation after verification/sync aborts before the manifest commit. Added a
  verification-cancellation regression test.
- Added a per-root display-only scan cache: the last library view is visible immediately on startup
  while the authoritative background scan runs. Move/Health/Doctor actions are blocked until the
  fresh scan finishes. The cache is not used for planning.
- Short status badges, populated empty-details summaries, drive-type/usage tooltips, game context
  menu and less-clipped Doctor/Health tables.
- Steam root and library discovery for native and Flatpak Steam.
- Local appmanifest scanning; no Steam API or network dependency.
- Shared-compatdata canonical prefix handling (skips self-moves).
- NTFS/exFAT/FAT32 filename, case and FAT32 size validation.
- Preflight blockers for updates, running Steam/Proton, payload/prefix conflicts,
  insufficient space, system components and unsafe destinations.
- Journaled copy/verify/sync/manifest/VDF transaction with backups, cancellation,
  rollback and a Recovery UI.
- NTFS Doctor for creating/repairing library compatdata symlinks.
- AppImage, AUR and gated Flatpak packaging templates.
- Tests for VDF/ACF/mount/filesystem rules, planner, executor, rollback and GUI startup.

Known limitations: destination libraries must already be registered; the GUI moves one
game at a time; crash-resume is not implemented (rollback before source-manifest removal is);
Flatpak host-process detection is not yet validated, so Flatpak is not shipped in the alpha release.
