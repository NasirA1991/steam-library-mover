## Summary

## Safety impact

- [ ] I read `docs/SAFETY.md`.
- [ ] I did not bypass planner/executor ordering or source-removal checks.
- [ ] I added tests for parser/planner/executor changes.

## Validation

- [ ] `ctest --test-dir build --output-on-failure`
- [ ] `desktop-file-validate resources/steam-library-mover.desktop`
- [ ] `appstreamcli validate --no-net resources/io.github.nasira1991.SteamLibraryMover.metainfo.xml`
