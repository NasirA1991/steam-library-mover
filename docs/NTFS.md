# NTFS, Proton and compatdata

Steam for Linux does not officially support game libraries on NTFS. Two independent problems show
up in practice:

1. **Proton prefixes contain filenames that NTFS cannot represent.** Wine creates symlinks such as
   `dosdevices/c:`; NTFS rejects `:` in filenames. If the prefix lives on NTFS, prefix creation or
   updates can fail, or `chkdsk` can remove links later.
2. **Steam's mover mishandles shared compatdata.** When `steamapps/compatdata` on the destination
   library is a symlink back to the prefix's current location, Steam tries to move the prefix onto
   itself. It logs `Can't overwrite existing file ".../compatdata/<appid>/..."` and then reports
   `content file locked`, rolling the game move back. See upstream issues
   [#12139](https://github.com/ValveSoftware/steam-for-linux/issues/12139) and
   [#12632](https://github.com/ValveSoftware/steam-for-linux/issues/12632).

## The recommended layout

Keep prefixes on a Linux filesystem and symlink the library's `compatdata` directory to that
location. For example:

```bash
shared="$HOME/.local/share/Steam/steamapps/compatdata"
library="/mnt/games/kingston/SteamLibrary/steamapps/compatdata"

if [ -L "$library" ]; then
    if [ "$(readlink -f -- "$library")" = "$(readlink -f -- "$shared")" ]; then
        echo "Already configured."
    else
        echo "Refusing: $library points somewhere else; leaving it unchanged." >&2
    fi
elif [ -e "$library" ]; then
    echo "Refusing: $library already exists; leaving it unchanged." >&2
    echo "If it is empty, inspect it and remove it with rmdir, then rerun." >&2
    echo "If it contains prefixes, migrate them first; never delete a non-empty directory." >&2
else
    mkdir -p -- "$shared"
    ln -s -- "$shared" "$library"
fi
```

The link target (`$shared` above) must be on ext4, btrfs, xfs or another Linux-compatible filesystem,
never on NTFS, exFAT or FAT32. Putting the target on another Windows-compatible filesystem does not
solve the prefix filename problem.

If `compatdata` already exists, choose the matching case:

- **Correct symlink:** the block reports “Already configured” and changes nothing.
- **Different symlink:** stop and inspect it. Do not overwrite it.
- **Empty real directory:** confirm it is empty with
  `find "$library" -mindepth 1 -maxdepth 1 -print -quit` (no output means empty), then
  `rmdir -- "$library"` and rerun the block. Do not use `rm -rf`.
- **Non-empty real directory:** do not delete it. Existing Proton prefixes must be migrated first,
  with Steam closed and a backup. Migration is not automatic: move the numbered prefix directories
  into `$shared`, preserving permissions, and verify the games before replacing `compatdata`. If you
  are unsure, leave the existing directory in place. The NTFS Doctor refuses to replace a non-empty
  prefix directory.

A bare `ln -s` is not safe when the destination already exists: it can create a second link inside
the existing directory instead of replacing it.

## What Steam Library Mover does

- Detects the library's compatdata type (missing, real directory, symlink, broken symlink).
- Resolves canonical source and destination prefix paths.
- Skips the prefix entirely when both resolve to the same directory.
- Blocks a move if a different prefix already exists at the destination.
- Blocks Proton prefixes whose effective location is a Windows-compatible filesystem.
- Refuses to create a `compatdata` symlink on a library while Steam is running.
- Never deletes the library-level compatdata symlink.
- Reports an existing valid symlink as configured, and treats selecting the current target as a
  no-op instead of recreating the link.

## Executable permissions

A typical NTFS mount (`uid=1000,gid=1000,umask=022`) does not preserve Unix executable bits. Proton
games usually work because Proton runs the Windows executables. Native Linux games may fail to launch
from such a library. The planner warns about this and the GUI requires an acknowledgement before
moving to a Windows-compatible filesystem.

Some ntfs-3g configurations use `permissions` or `showexec`; the Doctor reports whether those options
are present. Steam itself requires an executable-enabled mount to add a library.

## Mount options and automatic mounts

The tool does not mount, remount or format drives. It only reads the current mount table. If a drive
is not mounted, mount it through KDE/udisks or `/etc/fstab` first. The library is not visible to the
tool when its mount is unavailable.

`windows_names` can prevent invalid names from being created by Proton; it may also cause Proton
prefix creation to fail. The symlink layout above is the preferred fix. Do not rely on deleting
`*.lock` files; see below.

## Dual-boot lock files

Sharing a Steam library with Windows is unsupported by Valve. Linux and Windows Steam can leave
`.lock`, `pfx.lock`, `dist.lock` and `unity.lock` files that cause “content file locked” errors when
the other OS sees them. **Tools → Library Health…** lists common lock/temp files and can move them
into hidden `.slm-quarantine` folders. It never deletes them and refuses while Steam or a Proton/Wine
game is running.

## exFAT and FAT32

exFAT behaves like NTFS for filename rules and executable bits. FAT32 adds a 4 GiB per-file limit and
is refused if any moved file exceeds it. For current games, exFAT/NTFS + a compatdata symlink are the
only semi-practical Windows-filesystem options; ext4/btrfs is strongly preferred.
