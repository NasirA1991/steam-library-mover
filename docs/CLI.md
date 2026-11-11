# Command-line reference

`steam-library-mover-cli` is the headless frontend to the same scanner, planner and move engine used
by the desktop application. It is intended for scripting, diagnostics and recovery on machines
without a desktop session.

The desktop application and CLI share a machine-wide lock. Only one GUI or CLI instance can hold it
at a time; the lock is acquired at startup and released when the process exits. This also applies to
commands that only read files.

```text
steam-library-mover-cli [options] [command]
```

If no command is given, `list` is used.

## Global options

| Option | Description |
| --- | --- |
| `-h`, `--help` | Show the global option list. |
| `-v`, `--version` | Print the application version. |
| `--steam-root <path>` | Use an explicit Steam installation root instead of auto-detection. |
| `--json` | Emit JSON for `list`, `plan`, `move`, `doctor` and `health`. Not used by `recover` or `fix-compatdata`. |

The parser accepts the move and maintenance options globally, so `--help` is not command-specific.
The tables below show where each option is meaningful.

## Commands

### `list`

Print detected libraries, filesystem types, free space, compatdata state and installed games. This
is the default command.

```bash
steam-library-mover-cli list
steam-library-mover-cli list --json
```

### `plan`

Build a move plan and show its components, storage projection, warnings and blockers. **No game
files are modified**, even without `--dry-run`.

```bash
steam-library-mover-cli plan --appid 346010 --to /mnt/games/SteamLibrary
```

Required: `--appid`, `--to`.

A plan with blockers returns exit code `2`. Warnings do not by themselves prevent a move, but read
them before continuing.

### `move`

Execute a planned move. The move is refused unless `--yes` is supplied.

```bash
steam-library-mover-cli move --appid 346010 --to /mnt/games/SteamLibrary --yes
```

Required: `--appid`, `--to`, `--yes`.

Before copying, the command revalidates the selected Steam installation, mounts, free space,
manifest state and running processes. Progress is written to standard output, or to standard error
when `--json` is used.

### `health`

Scan libraries for stale lock files, stale manifest temporary files, orphaned payloads, prefixes,
shader cache and Workshop data, duplicate appids, broken compatdata links and source backups.

```bash
steam-library-mover-cli health
steam-library-mover-cli health --json
```

With `--apply --yes`, quarantineable lock/temp files are renamed into a hidden `.slm-quarantine`
directory next to the original file. Quarantine never deletes files and is refused as root or while
Steam or a Proton/Wine game is running.

`health` exits `0` when the scan completes, even if it finds problems. Findings are informational;
review them and use `--apply` only for files you intend to move aside. `--apply` without `--yes`
returns `1` and changes nothing.

### `doctor`

Inspect NTFS/exFAT compatibility and Proton prefix placement for each library. This command is
read-only; it prints findings and the suggested shared compatdata target.

```bash
steam-library-mover-cli doctor
steam-library-mover-cli doctor --json
```

### `fix-compatdata`

Create or replace a library's `compatdata` symlink so it points at a Linux filesystem. This is a dry
run unless `--apply` is supplied.

```bash
# Show the current state and the default target
steam-library-mover-cli fix-compatdata --library /mnt/games/SteamLibrary

# Apply it to an explicit target
steam-library-mover-cli fix-compatdata \
  --library /mnt/games/SteamLibrary \
  --to ~/.local/share/Steam/steamapps/compatdata \
  --apply
```

Required: `--library`. Optional: `--to` (defaults to the Doctor's suggested target), `--apply`.
Steam must be closed; the command returns `2` if it detects a running Steam.

### `recover`

Inspect, resume or roll back an interrupted operation.

```bash
steam-library-mover-cli recover --list
steam-library-mover-cli recover --resume <operation-id>
steam-library-mover-cli recover --rollback <operation-id>
```

Use exactly one of `--list`, `--resume` or `--rollback`. Resume is available only for operations
interrupted before the Steam manifest commit. Rollback is available only while the source manifest
still exists. Both commands refuse to run while Steam or a game is using the affected data.

The desktop **Recovery** dialog provides the same operations with a progress window and access to
the operation log. For a long resume, prefer the desktop application.

## Options for `plan` and `move`

| Option | Description |
| --- | --- |
| `--appid <id>` | Decimal Steam application id. Required. |
| `--to <path>` | Existing destination Steam library. Required. |
| `--dry-run` | With `move`, build and print the plan without touching files. |
| `--yes` | Confirm the move. Without it, `move` refuses with exit code `2`. |
| `--verify full\|quick` | Default `full` (SHA-256). `quick` checks paths and sizes only; it cannot detect same-size corruption. |
| `--no-shadercache` | Do not move the shader cache. |
| `--no-workshop` | Do not move Workshop content and its manifest. |
| `--allow-ntfs` | Acknowledge the Windows-filesystem risk, including missing native Linux executable bits. |
| `--quit-steam` | Ask the selected Steam client to shut down, wait briefly, then continue planning. This is a request, not a forced kill. |
| `--keep-source` | Register at the destination but keep moved source data as hidden `.slm-backup-*`; source space is not freed until you remove them. |

The plan and move refuse to run as root. A move also refuses if Steam is running; use
`--quit-steam` or close Steam first.

## Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Command completed successfully. |
| `1` | Usage error, invalid option, scan failure, missing operation, or another command error. |
| `2` | Safety refusal: plan blockers, missing `--yes`, or Steam running when it must be closed. |
| `3` | The move started but failed during execution. Inspect the printed journal path and use `recover --list`. |

Exit code `2` is a safety refusal, not a crash. `health` uses `1` when `--apply` is requested without
`--yes`; it does not use `2` for that case.

## JSON output

`--json` prints a single JSON document to standard output for:

- `list` — Steam root, libraries, games and scan issues;
- `plan` and `move` — the move plan, components and issues;
- `doctor` — Doctor issues;
- `health` — health findings.

`move --json` writes human-readable progress to standard error, so the JSON on standard output is
not mixed with progress text. `recover` and `fix-compatdata` do not produce JSON.

Examples:

```bash
steam-library-mover-cli list --json > libraries.json
steam-library-mover-cli plan --appid 346010 --to /mnt/games/SteamLibrary --json > plan.json
```

## Environment variables

| Variable | Effect |
| --- | --- |
| `SLM_STEAM_ROOT` | Default Steam root when `--steam-root` is not supplied. |
| `SLM_STATE_DIR` | Base directory for journals, logs, backups and recovery state. |
| `SLM_CACHE_DIR` | Directory for the display-only startup scan cache. |
| `SLM_ARTWORK_DIR` | Local box-art cache directory (GUI). |
| `SLM_DISABLE_ARTWORK` | Disable box-art downloads (GUI). |
| `SLM_THEME` | `system`, `light` or `dark` for the current GUI run. |

## Files

| Path | Contents |
| --- | --- |
| `~/.local/state/steam-library-mover/operations/<opId>/journal.json` | Operation phase, paths, component state and hashes. |
| `~/.local/state/steam-library-mover/operations/<opId>/log.txt` | Human-readable operation log. |
| `~/.local/state/steam-library-mover/operations/<opId>/backup/` | Original `libraryfolders.vdf` and game manifest. |
| `~/.cache/steam-library-mover/` | Display-only scan cache and box art. |

Journal and log files are mode `0600` and can contain local paths. Redact paths before posting them
in an issue. Finished journals are pruned to the 20 most recent; unfinished, failed and cancelled
operations are retained for Recovery.

## See also

- [Installation](INSTALL.md)
- [Safety model](SAFETY.md)
- [NTFS libraries](NTFS.md)
- [Architecture](ARCHITECTURE.md)
