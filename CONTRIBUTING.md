# Contributing

1. Read `docs/SAFETY.md` first. Changes that weaken the transaction ordering or deletion
   checks need a very good reason.
2. Build and run the tests:
   ```bash
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
   cmake --build build
   ctest --test-dir build --output-on-failure
   ```
3. Add tests for parser, planner or executor changes. Use fake mount tables and temporary
   Steam roots; never point tests at real game libraries.
4. Keep all destructive operations behind the planner/executor. The GUI must not copy or
   delete files itself.
5. Do not introduce network calls without an explicit, opt-in privacy discussion.
6. Packaging changes should keep `desktop-file-validate`, `appstreamcli validate --no-net`
   and the AppImage/AUR builds clean.

The project is GPL-3.0-or-later. By contributing you agree to license your changes under it.
