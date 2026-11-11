# Roadmap

Steam Library Mover is a public alpha project. The roadmap is a record of priorities and boundaries,
not a dated delivery schedule. Safety, data preservation and recoverability come before new
convenience features.

Current public release: **v0.2.0**. Shipped behavior is documented in the
[release notes](RELEASE_NOTES.md) and [changelog](../CHANGELOG.md); this page describes what is
being considered next and what the project deliberately does not do.

## Current baseline

The following are implemented and supported in the current release:

- Review-before-move planning with blockers, storage projections and NTFS/exFAT/FAT32 checks.
- Journaled copy → verify → sync → manifest/VDF commit → source cleanup.
- Resume of operations interrupted before the manifest commit, including one automatic
  remove-and-recopy repair of a failed verification.
- Rollback while the source manifest still exists.
- Machine-wide instance lock and mid-copy Steam/Proton/Wine process monitoring.
- Full SHA-256 verification by default; quick verification as an explicit opt-out.
- Copy-time source hashing with destination-only full verification when source metadata is unchanged.
- Sparse-file preservation, bounded parallel copying and parallel verification, atomic file rename
  completion and sequential-I/O advice.
- Per-game selection and fresh preflight in the batch review; persistent window layout; verification
  progress.
- Batch moves, pause/resume, cancellation, keep-source backups and optional reflink copies.
- Library Health scanning and quarantine of stale lock/temp files.
- NTFS Doctor and shared `compatdata` symlink setup.
- GUI, CLI, desktop notifications, themes and sleep inhibition.
- AUR and AppImage release artifacts, with build-provenance attestations.

## Next candidates

These are candidates, not commitments. Work is accepted when it can preserve the transaction and
recovery guarantees and can be tested without risking a real library.

### Performance and copy efficiency

- Per-file hashes stored in the journal for faster verification and reliable resume.
- `copy_file_range` fallback and further I/O advice tuning.
- Cache per-filesystem-pair reflink support instead of retrying known-unsupported clones.
- Reuse the planned file tree after cheap revalidation.

### Reliability and testing

- Crash injection across every executor phase, including power-loss-style interruptions.
- Privileged loopback NTFS integration in CI.
- More fault-injection tests for deletion, rollback and recovery edge cases.
- Flatpak host-process detection and shutdown validation, if a confined build can be made safe.

### Usability and maintainability

- Continue splitting the main window into focused dialogs and a transfer-queue controller.
- Complete settings persistence and review keyboard/accessibility behavior.
- Translation-ready strings.
- Clearer recovery descriptions.

## Deliberately deferred

These ideas may be useful, but the current design is not ready for them. They need a separate safety
case and tests before implementation.

- **Same-filesystem directory rename.** Whole-directory renames conflict with the per-component
  journal and rollback model and can fail across Btrfs subvolumes. The reflink path captures much of
  the speed benefit without changing rollback semantics.
- **Undo after a completed move.** A reliable reverse operation needs a stronger reverse journal and
  crash tests. Resume before the manifest commit is supported; one-click undo is not.
- **Guided destructive repairs.** Library Health detects and explains orphaned payloads, prefixes and
  caches. Automatically deleting them needs a preview and a journaled operation.
- **Library identity and automatic relocation.** Rewriting library paths in Steam metadata is a
  database mutation and needs multi-mount testing.
- **Prefix or launch-option path rewriting.** Detection should come before rewriting; a bad rewrite
  can break saves or wrappers.
- **Scheduled moves, SMART-driven tiering, D-Bus automation.** These add automation risk and should
  stay reviewable rather than silent.
- **Flatpak as a release artifact.** The experimental build is not shipped until host process
  detection and sandbox permissions are validated.

## Non-goals

- Creating or registering new Steam libraries or adding drive entries to Steam's database.
- Downloading, validating or adopting games outside Steam's ownership and manifest model.
- Bypassing Steam's library management.
- Managing non-Steam launchers in the core mover.
- Reading or transmitting Steam credentials, tokens or cloud-save data.

## Decision principles

1. Do not modify a library until the plan is explicit and the destination is safe.
2. Keep the source authoritative until the destination is verified and the manifest commit succeeds.
3. Refuse ambiguous filesystems, paths and process states rather than guessing.
4. Make every destructive action journaled, recoverable or both.
5. Prefer small, testable changes over broad automation.
