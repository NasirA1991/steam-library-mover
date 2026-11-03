# Security policy

Steam Library Mover performs file moves and Steam metadata updates. Treat a bug that can delete,
overwrite or misdirect game data as a security issue, not only as a normal bug.

## Reporting

Please report security-sensitive issues privately through GitHub Security Advisories for
`NasirA1991/steam-library-mover`, or email **evilleader91@gmail.com** with `[steam-library-mover
security]` in the subject.

Include:

- the version or commit;
- the operation you attempted;
- the affected filesystem and mount state;
- whether Steam was running;
- relevant journal/log excerpts with personal paths redacted.

Please do not publish exploit details for a destructive data-loss issue until a fix is available.

## Scope

The application does not use telemetry and does not require a Steam API key. Optional box-art
downloads use Steam's public CDN; they can be disabled in Settings. Credentials and Steam login
tokens are not read or transmitted by this project.
