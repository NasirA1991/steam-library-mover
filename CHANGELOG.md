# Changelog

## 0.2.0 — unreleased

- The main window remembers its size and position across launches.
- Sequential copy reads/writes use `posix_fadvise` to reduce readahead and keep writeback in
  order; copied source pages are dropped from the page cache.
- Copy completion replaces files and symlinks with an atomic `rename(2)` instead of unlinking
  the old destination first.
- Full verification now reports hashed bytes instead of showing an indeterminate progress bar.
- Sparse source files are copied by data extent with `SEEK_HOLE`/`SEEK_DATA`, so holes are not
  materialized as allocated zero blocks when the filesystem supports them.
- The library sidebar can be hidden from View → Library sidebar (Ctrl+B), and the minimum window
  size is now 900×560 so the window fits smaller displays.

## 0.1.9 — 2026-11-11

- Recovery can resume an interrupted copy before the manifest commit. Verified destination
  components are reused; incomplete or corrupt components are removed and copied again.
- A failed verification now triggers one automatic remove-and-recopy repair before the operation
  is marked failed.
- Resume refuses changed source manifests or library databases, and validates journaled destination
  paths against the destination library.
- Recursive cleanup is now descriptor-relative, does not follow symlinks, and refuses to cross a
  filesystem boundary, closing a path-swap race during source cleanup and rollback.
- A machine-wide advisory lock now prevents the GUI and CLI (including different users) from
  starting overlapping operations. Steam, game and Proton/Wine processes are also polled during
  copy and verification; if one starts, the move stops before commit and can be resumed.
- Process polling and the graceful Steam shutdown request now run off the GUI thread, so the
  window stays responsive while Steam exits.
- Journals for finished moves are pruned to the 20 most recent after a move; unfinished,
  failed and cancelled operations are retained until you recover or forget them.
- Release workflows pin third-party actions to commit SHAs and packaging tools to tagged
  releases with verified checksums; release artifacts carry GitHub build-provenance attestations.

## 0.1.8 — 2026-11-11

- The NTFS acknowledgement can no longer be bypassed by reopening the review
  dialog; the core planner also detects unacknowledged Windows filesystems.
- Disk mounts are reloaded and revalidated at execute time and immediately
  before the manifest commit.
- Recovery now offers View log, Open folder and Forget actions instead of dead
  ends.
- Pause/resume speed and ETA no longer include time spent paused.
- VDF parsing rejects unterminated quotes; planner preflights unsupported file
  types and warns about volatile destinations.
- Runtime detection no longer treats arbitrary `proton*` names as system
  components, and VDF app sizes use the scanned game component.
- Artwork downloads are size-capped and content-validated; Steam launch uses
  known installation paths before `PATH`.

## 0.1.7 — 2026-11-08

- An existing destination shader cache no longer blocks the move. The existing
  cache is kept, the source cache is left behind, and the review shows a warning.
- Full SHA-256 verification is labelled as the default rather than mandatory;
  unchecking it does not gate Start.
- The review dialog now explains why Start is disabled.
- GUI tests isolate `QSettings` from the real user configuration.

## 0.1.6 — 2026-11-08

- Full SHA-256 verification is now the default. Quick mode is an explicit opt-out
  with a visible risk warning in the GUI and CLI.
- Rollback restores `libraryfolders.vdf` before deleting destination data, validates
  operation IDs and journal paths, and refuses unsafe backups.
- NTFS acknowledgement is collected by the GUI but enforced by the core planner and
  executor.
- Unknown filesystems are refused; ambiguous `fuseblk` mounts carry a warning.
- Source cleanup no longer falls back to deleting in place when quarantine rename fails.
- Shared GUI/CLI instance lock, journal fsync, safer process detection, valid CLI JSON
  output, mount revalidation, traversal depth limits and copy-buffer reuse.
- Release Arch and AppImage builds run tests; root-hostile tests skip safely.
- Packaging documentation now matches the automated release and AUR workflows.

## 0.1.5 — 2026-11-05

- Added a test-only process-inspector seam so package tests do not fail when
  Steam is already running on the build host. No runtime behavior change.

## 0.1.4 — 2026-11-05

- Fixed relative symlink corruption: targets are now captured with `readlink` and
  recreated verbatim instead of being silently converted to absolute source paths.
- Added a regression test covering relative symlinks and `../` targets.
- Treated `done-with-warnings` operations as finished so they stop appearing as
  unfinished moves forever.
- Rollback now refuses to run as root and refuses while Steam, a prefix or a game
  directory is in use.
- Removed the `org.freedesktop.Flatpak` permission from the experimental Flatpak
  manifest; host command execution must not be implicit.
- Removed tag interpolation in the AUR and Arch package workflow shell commands.
- Centralized the application version in CMake and generated the manpage version.

## 0.1.3 — 2026-11-04

- Added **Open on ProtonDB** to the game context menu for Proton and unknown-runtime games.
  Native games and tools only show the existing SteamDB link.

## 0.1.2 — 2026-11-04

- About dialog now shows the application version and clickable GitHub, AUR and issue-tracker links.

## 0.1.1 — 2026-11-04

Packaging and release automation release. No changes to the move engine or desktop UI.

- GitHub Actions release workflow builds and publishes AppImage, Arch package and source archive.
- Added README screenshots and build/install documentation.
- Added experimental Flatpak source build as a GitHub Actions artifact.
- Added automatic AUR publishing on `v*` tags.
- Fixed the GUI test race on slow/headless startup and bounded test runtimes.

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
