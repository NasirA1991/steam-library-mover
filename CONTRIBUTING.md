# Contributing

Steam Library Mover is safety-critical file-management software. A change that looks small can
delete or misdirect a game library, so the project prefers conservative changes, explicit failure
and tests over clever shortcuts.

## Before you start

Read the [safety model](docs/SAFETY.md) and the [architecture overview](docs/ARCHITECTURE.md).
Changes that weaken the transaction order, deletion containment, process checks or verification
barriers need a clear justification and focused tests.

For security-sensitive issues, do **not** open a public issue. Follow [SECURITY.md](SECURITY.md).

## Development setup

Build requirements and distribution package names are in the [installation guide](docs/INSTALL.md).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure --timeout 60
```

Do not run the test suite as root. Tests use temporary Steam roots and injectable mount tables; they
must never require a real game library.

For a release-style build:

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure --timeout 60
```

## Project layout

| Path | Purpose |
| --- | --- |
| `src/core/` | Scanner, planner, executor, copy engine, journal, health and shared data model. This layer is Qt Core only and contains all filesystem mutation. |
| `src/cli/` | Headless frontend. |
| `src/gui/` | Qt Widgets frontend and worker threads. It must not copy or delete files directly. |
| `tests/` | Qt Test suites; see [docs/TESTING.md](docs/TESTING.md). |
| `resources/` | Desktop entry, icon, AppStream metadata and man page. |
| `packaging/` | AUR, AppImage and experimental Flatpak recipes. |
| `docs/` | User, safety and maintainer documentation. |

## Making changes

1. Keep all destructive operations behind the planner/executor. The GUI and CLI must not implement
   their own copy, delete or manifest-edit paths.
2. Add tests for parser, planner, executor or filesystem changes. Use fake mount tables and
   temporary Steam roots; never point a test at real game libraries.
3. Update the relevant documentation when behavior, options, files or limitations change:
   README, [CLI reference](docs/CLI.md), [safety model](docs/SAFETY.md), [testing](docs/TESTING.md)
   or the man page.
4. Do not introduce network calls without an explicit privacy discussion and an opt-out.
5. Do not add a dependency that is not needed for the core move or desktop integration. Qt 6.4+ is
   the compatibility floor.
6. Keep public behavior conservative: refuse ambiguous filesystems and states instead of guessing.

## Validation

Before opening a pull request, run:

```bash
ctest --test-dir build --output-on-failure --timeout 60

desktop-file-validate resources/steam-library-mover.desktop
appstreamcli validate --no-net resources/io.github.nasira1991.SteamLibraryMover.metainfo.xml

git diff --check
```

`desktop-file-validate` and `appstreamcli` are distribution packages; CI installs them for the
Ubuntu job. If they are unavailable locally, state that in the pull request instead of claiming the
checks passed.

Manual NTFS testing and real-machine acceptance are described in
[docs/TESTING.md](docs/TESTING.md). The loopback NTFS script requires root and is intended for a
developer machine, not CI.

## Pull requests

- Keep changes focused. Separate refactors, documentation and behavior changes when practical.
- Describe the user-visible effect and any safety impact.
- List the tests and manual checks you ran.
- Mention new limitations, blockers or recovery behavior.
- Do not include personal paths, game library contents, tokens, logs with private data, build trees
  or local screenshots.
- Use a descriptive commit message. Imperative subject lines match the existing history.

## License

The project is GPL-3.0-or-later. By contributing, you agree to license your changes under the same
terms. Not affiliated with Valve Corporation.
