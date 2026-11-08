// SPDX-License-Identifier: GPL-3.0-or-later
#include "planner.h"

#include "appmanifest.h"
#include "filetree.h"
#include "library.h"
#include "mountinfo.h"
#include "process_check.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <algorithm>

#include <cmath>

#include <sys/stat.h>

#include <unistd.h>

namespace slm {

namespace {

constexpr quint64 kReserveMinimum = 256ull * 1024ull * 1024ull;
constexpr double kReserveFraction = 0.02;

quint64 deviceFor(const QString &path)
{
    struct stat st {};
    if (path.isEmpty() || ::stat(QFile::encodeName(path).constData(), &st) != 0)
        return 0;
    return static_cast<quint64>(st.st_dev);
}

QString joinPath(const QString &base, const QString &name)
{
    if (base.endsWith(QLatin1Char('/')))
        return base + name;
    return base + QLatin1Char('/') + name;
}

void addIssue(MovePlan &plan, Severity severity, const QString &code, const QString &message,
              const QString &detail = {})
{
    plan.issues.append({severity, code, message, detail});
}

bool destinationExists(const QString &path)
{
    return pathExistsNoFollow(path);
}

quint64 reserveFor(quint64 bytes)
{
    const quint64 fraction = static_cast<quint64>(std::ceil(static_cast<double>(bytes) * kReserveFraction));
    return std::max(kReserveMinimum, fraction);
}

void addBytesForMount(MovePlan &plan, const MountTable &mounts, const QString &destination,
                      quint64 bytes)
{
    if (bytes == 0)
        return;
    MountInfo mount = mounts.forPath(destination);
    const QString key = mount.mountPoint.isEmpty() ? destination : mount.mountPoint;
    plan.bytesPerMount[key] += bytes;
    plan.freePerMount.insert(key, mount.freeBytes);
}

void addTreeComponent(MovePlan &plan, const MountTable &mounts, ComponentKind kind,
                      const QString &source, const QString &destination, bool required,
                      bool windowsDestination, bool fat32Destination, const QString &note = {})
{
    FileTree tree = FileTree::scan(source);
    if (!tree.ok) {
        addIssue(plan, Severity::Blocker, QStringLiteral("scan-failed"),
                 QStringLiteral("Cannot scan %1").arg(componentKindName(kind)), tree.error);
        return;
    }
    if (destinationExists(destination)) {
        addIssue(plan, Severity::Blocker, QStringLiteral("destination-exists"),
                 QStringLiteral("%1 already exists at the destination").arg(componentKindName(kind)),
                 destination);
        return;
    }

    if (windowsDestination)
        validateTreeForWindows(tree, fat32Destination, &plan.issues);

    Component component;
    component.kind = kind;
    component.source = source;
    component.destination = destination;
    component.bytes = tree.uniqueBytes;
    component.required = required;
    component.note = note;
    plan.components.append(component);
    addBytesForMount(plan, mounts, destination, component.bytes);
}

void addFileComponent(MovePlan &plan, const MountTable &mounts, ComponentKind kind,
                      const QString &source, const QString &destination, bool required,
                      const QString &note = {})
{
    const QFileInfo info(source);
    if (!info.exists() || !info.isFile()) {
        if (required) {
            addIssue(plan, Severity::Blocker, QStringLiteral("source-missing"),
                     QStringLiteral("%1 is missing").arg(componentKindName(kind)), source);
        }
        return;
    }
    if (destinationExists(destination)) {
        addIssue(plan, Severity::Blocker, QStringLiteral("destination-exists"),
                 QStringLiteral("%1 already exists at the destination").arg(componentKindName(kind)),
                 destination);
        return;
    }
    Component component;
    component.kind = kind;
    component.source = source;
    component.destination = destination;
    component.bytes = static_cast<quint64>(info.size());
    component.required = required;
    component.note = note;
    plan.components.append(component);
    addBytesForMount(plan, mounts, destination, component.bytes);
}

QString sourcePrefixPath(const Library &library, quint32 appId, bool *exists, QString *error)
{
    if (library.compatdata.state == CompatDataState::BrokenSymlink) {
        if (error)
            *error = library.compatdata.error;
        if (exists)
            *exists = false;
        return {};
    }
    QString prefix = resolvePrefixPath(library, appId, exists);
    const QFileInfo info(prefix);
    if (info.isSymLink()) {
        if (error)
            *error = QStringLiteral("The per-app Proton prefix is itself a symlink; "
                                    "this is not supported for a move.");
        if (exists)
            *exists = false;
        return prefix;
    }
    return prefix;
}

QString destinationPrefixPath(const Library &library, quint32 appId, bool *exists, QString *error)
{
    if (library.compatdata.state == CompatDataState::BrokenSymlink) {
        if (error)
            *error = library.compatdata.error;
        if (exists)
            *exists = false;
        return {};
    }
    if (library.compatdata.state == CompatDataState::Symlink
        && library.compatdata.target.isEmpty()) {
        if (error)
            *error = QStringLiteral("The destination compatdata symlink has no valid target.");
        if (exists)
            *exists = false;
        return {};
    }
    QString prefix = resolvePrefixPath(library, appId, exists);
    return prefix;
}

} // namespace

QStringList MovePlanner::processPrefixPaths(const SteamScan &scan, const Game &game,
                                           const Library &destLibrary)
{
    QStringList prefixes;
    const Library *source = scan.findLibrary(game.libraryPath);
    bool exists = false;
    QString error;
    if (source) {
        const QString sourcePrefix = sourcePrefixPath(*source, game.appId, &exists, &error);
        if (!sourcePrefix.isEmpty())
            prefixes.append(sourcePrefix);
    }
    const QString destPrefix = destinationPrefixPath(destLibrary, game.appId, &exists, &error);
    if (!destPrefix.isEmpty())
        prefixes.append(destPrefix);
    prefixes.removeDuplicates();
    return prefixes;
}

MovePlan MovePlanner::plan(const SteamScan &scan, quint32 appId, const QString &destinationLibrary,
                           const MoveOptions &options, const MountTable &mounts, QString *error,
                           const QMap<QString, quint64> &reservedBytes)
{
    MovePlan plan;
    plan.options = options;
    plan.destLibraryPath = canonicalOrAbsolute(destinationLibrary);

    const Game *game = scan.findGame(appId);
    if (!game) {
        if (error)
            *error = QStringLiteral("App %1 is not installed in this Steam installation").arg(appId);
        return plan;
    }
    plan.game = *game;
    plan.issues = game->issues;

    if (::geteuid() == 0) {
        addIssue(plan, Severity::Blocker, QStringLiteral("running-as-root"),
                 QStringLiteral("Do not run this tool as root"),
                 QStringLiteral("Files would become root-owned and could break Steam. "
                                "Run it as the desktop user that owns the libraries."));
    }

    const Library *source = scan.findLibrary(game->libraryPath);
    const Library *dest = scan.findLibrary(destinationLibrary);
    if (!source) {
        addIssue(plan, Severity::Blocker, QStringLiteral("source-library-missing"),
                 QStringLiteral("Source library not found"), game->libraryPath);
        if (error)
            *error = QStringLiteral("Source library not found");
        return plan;
    }
    plan.sourceLibraryPath = source->path;
    plan.libraryFoldersPath = scan.libraryFoldersPath;

    if (!dest) {
        addIssue(plan, Severity::Blocker, QStringLiteral("destination-library-unknown"),
                 QStringLiteral("Destination is not a registered Steam library"),
                 destinationLibrary);
        if (error)
            *error = QStringLiteral("Destination library not found");
        return plan;
    }
    plan.destLibraryPath = dest->path;

    for (const Issue &issue : scan.issues) {
        if (issue.isBlocker())
            plan.issues.append(issue);
    }

    if (scan.root.kind == InstallKind::Snap) {
        addIssue(plan, Severity::Blocker, QStringLiteral("snap-unsupported"),
                 QStringLiteral("Snap Steam is not supported"),
                 QStringLiteral("Snap confinement makes library access unreliable. Use native or Flatpak Steam."));
    }
    if (game->category == GameCategory::SystemComponent) {
        addIssue(plan, Severity::Blocker, QStringLiteral("system-component"),
                 QStringLiteral("This is a Steam runtime/component, not a game"),
                 QStringLiteral("Moving it would break every game that depends on it."));
    } else if (game->category == GameCategory::Broken
               || game->category == GameCategory::Unmanaged) {
        addIssue(plan, Severity::Blocker, QStringLiteral("not-movable"),
                 QStringLiteral("This entry is not a movable installed game"));
    } else if (game->category == GameCategory::Tool) {
        addIssue(plan, Severity::Warning, QStringLiteral("tool"),
                 QStringLiteral("This is a tool/redistributable, not a game"),
                 QStringLiteral("Moving it is allowed, but verify it is not shared by other games."));
    }

    if (source->path == dest->path) {
        addIssue(plan, Severity::Blocker, QStringLiteral("same-library"),
                 QStringLiteral("Source and destination are the same Steam library"));
    }
    if (source->vdfKey == QStringLiteral("__root")
        || dest->vdfKey == QStringLiteral("__root")) {
        addIssue(plan, Severity::Blocker, QStringLiteral("unregistered-library"),
                 QStringLiteral("A library is not listed in libraryfolders.vdf"),
                 QStringLiteral("Start Steam once so it registers the library, then rescan."));
    }
    if (source->mount.mountPoint.isEmpty() || dest->mount.mountPoint.isEmpty()) {
        addIssue(plan, Severity::Blocker, QStringLiteral("mount-unknown"),
                 QStringLiteral("Cannot determine the filesystem for a library"));
    }
    if (source->mount.readOnly) {
        addIssue(plan, Severity::Blocker, QStringLiteral("source-readonly"),
                 QStringLiteral("The source library is mounted read-only"));
    }
    if (dest->mount.readOnly) {
        addIssue(plan, Severity::Blocker, QStringLiteral("destination-readonly"),
                 QStringLiteral("The destination library is mounted read-only"));
    }
    if (source->mount.isNetwork() || dest->mount.isNetwork()) {
        addIssue(plan, Severity::Blocker, QStringLiteral("network-filesystem"),
                 QStringLiteral("Network filesystems are not supported"));
    }
    if (!source->writeAccess) {
        addIssue(plan, Severity::Blocker, QStringLiteral("source-not-writable"),
                 QStringLiteral("The source library is not writable"),
                 QStringLiteral("The tool needs permission to remove the source manifest and payload."));
    }
    if (!dest->writeAccess) {
        addIssue(plan, Severity::Blocker, QStringLiteral("destination-not-writable"),
                 QStringLiteral("The destination library is not writable"));
    }
    if (dest->mount.fsClass == FsClass::Other) {
        addIssue(plan, Severity::Blocker, QStringLiteral("unknown-filesystem"),
                 QStringLiteral("The destination filesystem type is not supported"),
                 QStringLiteral("%1. Use a recognized native, NTFS or exFAT filesystem.")
                     .arg(dest->mount.describe()));
    }
    if (dest->mount.fsType.compare(QStringLiteral("fuseblk"), Qt::CaseInsensitive) == 0) {
        addIssue(plan, Severity::Warning, QStringLiteral("fuseblk-ambiguous"),
                 QStringLiteral("The destination is a FUSE block filesystem"),
                 QStringLiteral("fuseblk can be NTFS, exFAT or FAT32; filesystem-specific limits "
                                "cannot be determined reliably from the mount type."));
    }
    if (dest->mount.isWindowsCompatible()) {
        Issue issue{Severity::Warning, QStringLiteral("windows-filesystem"),
                    QStringLiteral("Destination is a Windows-compatible filesystem"),
                    QStringLiteral("Steam does not officially support this. Proton games usually work; "
                                   "native Linux games may lose executable permissions or fail to launch. "
                                   "Mount options: %1")
                        .arg(dest->mount.options.join(QStringLiteral(", ")))};
        if (!options.allowNtfsNative) {
            issue.severity = Severity::Blocker;
            issue.code = QStringLiteral("windows-filesystem-ack");
            issue.message = QStringLiteral("Acknowledge the Windows-filesystem warning first");
            issue.detail = QStringLiteral("Confirm that you understand native Linux games may not run "
                                          "from this destination before moving.");
        }
        plan.issues.append(issue);
        if (!dest->mount.hasOption(QStringLiteral("permissions"))
            && !dest->mount.hasOption(QStringLiteral("showexec"))) {
            addIssue(plan, Severity::Warning, QStringLiteral("ntfs-exec-bits"),
                     QStringLiteral("The destination mount does not preserve executable bits"),
                     QStringLiteral("Expected for ntfs-3g: uid/gid plus permissions, or showexec. "
                                    "This is fine for most Proton games but not native Linux games."));
        }
    }
    if (source->mount.isWindowsCompatible()) {
        addIssue(plan, Severity::Warning, QStringLiteral("source-windows-fs"),
                 QStringLiteral("Source game files are on a Windows-compatible filesystem"),
                 QStringLiteral("Proton prefixes should be stored on a Linux filesystem. "
                                "The move will verify the source before deleting it."));
    }

    const QString sourceGame = joinPath(joinPath(source->steamappsPath(), QStringLiteral("common")),
                                        game->installDir);
    const QString destGame = joinPath(joinPath(dest->steamappsPath(), QStringLiteral("common")),
                                      game->installDir);

    FileTree gameTree = FileTree::scan(sourceGame);
    if (!gameTree.ok) {
        addIssue(plan, Severity::Blocker, QStringLiteral("scan-game-failed"),
                 QStringLiteral("Cannot scan the game directory"), gameTree.error);
    } else {
        if (destinationExists(destGame)) {
            addIssue(plan, Severity::Blocker, QStringLiteral("destination-game-exists"),
                     QStringLiteral("The game directory already exists at the destination"),
                     destGame);
        }
        if (dest->mount.isWindowsCompatible()) {
            QString reason;
            if (!isValidWindowsComponent(game->installDir, &reason))
                addIssue(plan, Severity::Blocker, QStringLiteral("ntfs-invalid-installdir"),
                         QStringLiteral("Game directory name is invalid on the destination"),
                         reason);
            validateTreeForWindows(gameTree, dest->mount.fsClass == FsClass::Fat32, &plan.issues);
        }
        Component gameComponent;
        gameComponent.kind = ComponentKind::Game;
        gameComponent.source = sourceGame;
        gameComponent.destination = destGame;
        gameComponent.bytes = gameTree.uniqueBytes;
        plan.components.append(gameComponent);
        addBytesForMount(plan, mounts, destGame, gameComponent.bytes);
    }

    if (game->sizeOnDisk > 0 && gameTree.ok) {
        const quint64 actual = gameTree.uniqueBytes;
        const quint64 manifestSize = game->sizeOnDisk;
        const double diff = manifestSize == 0
            ? 0.0
            : std::abs(static_cast<double>(actual) - static_cast<double>(manifestSize))
                / static_cast<double>(manifestSize);
        if (diff > 0.05) {
            addIssue(plan, Severity::Warning, QStringLiteral("size-mismatch"),
                     QStringLiteral("Installed size differs from the manifest"),
                     QStringLiteral("Manifest reports %1; scanned %2. The move uses the scanned size.")
                         .arg(formatBytes(manifestSize), formatBytes(actual)));
        }
    }

    // Proton prefix resolution.
    QString prefixError;
    bool sourcePrefixExists = false;
    const QString resolvedSourcePrefix = sourcePrefixPath(*source, appId, &sourcePrefixExists, &prefixError);
    bool destPrefixExists = false;
    QString destPrefixError;
    const QString resolvedDestPrefix = destinationPrefixPath(*dest, appId, &destPrefixExists, &destPrefixError);

    if (!resolvedSourcePrefix.isEmpty()) {
        const QFileInfo sourcePrefixInfo(resolvedSourcePrefix);
        if (sourcePrefixInfo.isSymLink()) {
            addIssue(plan, Severity::Blocker, QStringLiteral("prefix-is-symlink"),
                     QStringLiteral("The per-app Proton prefix is a symlink"),
                     QStringLiteral("Resolve the symlink manually before moving; the mover will not "
                                    "delete or follow it."));
            sourcePrefixExists = false;
        } else if (sourcePrefixInfo.exists() && !sourcePrefixInfo.isDir()) {
            addIssue(plan, Severity::Blocker, QStringLiteral("prefix-not-dir"),
                     QStringLiteral("The Proton prefix is not a directory"), resolvedSourcePrefix);
            sourcePrefixExists = false;
        }
    }

    if (dest->compatdata.state == CompatDataState::BrokenSymlink) {
        addIssue(plan, Severity::Blocker, QStringLiteral("compatdata-broken"),
                 QStringLiteral("The destination compatdata symlink is broken"),
                 dest->compatdata.error);
    }
    if (dest->mount.isWindowsCompatible()
        && dest->compatdata.state == CompatDataState::Missing) {
        addIssue(plan, Severity::Blocker, QStringLiteral("compatdata-needed"),
                 QStringLiteral("This NTFS library has no safe compatdata symlink"),
                 QStringLiteral("Run the NTFS Doctor to point %1 to a Linux filesystem before moving "
                                "a Proton game here.").arg(dest->compatdataPath()));
    }
    if (dest->compatdata.targetOnWindowsFs) {
        addIssue(plan, Severity::Blocker, QStringLiteral("compatdata-on-windows-fs"),
                 QStringLiteral("The destination Proton prefix would live on a Windows-compatible filesystem"),
                 QStringLiteral("This can corrupt Proton data through invalid filenames."));
    }
    if (source->compatdata.targetOnWindowsFs) {
        addIssue(plan, Severity::Warning, QStringLiteral("source-prefix-on-windows-fs"),
                 QStringLiteral("The existing Proton prefix is on a Windows-compatible filesystem"),
                 QStringLiteral("The move can relocate it to a safe destination, but the source "
                                "prefix may already have issues."));
    }

    const QString sourcePrefixCanonical = resolvedSourcePrefix.isEmpty()
        ? QString()
        : canonicalOrAbsolute(resolvedSourcePrefix);
    const QString destPrefixCanonical = resolvedDestPrefix.isEmpty()
        ? QString()
        : canonicalOrAbsolute(resolvedDestPrefix);

    if (sourcePrefixExists && destPrefixExists && sourcePrefixCanonical == destPrefixCanonical) {
        Component component;
        component.kind = ComponentKind::Prefix;
        component.source = resolvedSourcePrefix;
        component.destination = resolvedDestPrefix;
        component.bytes = 0;
        component.skip = true;
        component.note = QStringLiteral("Source and destination compatdata resolve to the same "
                                        "directory; the prefix will be left untouched.");
        plan.components.append(component);
    } else if (sourcePrefixExists && destPrefixExists) {
        addIssue(plan, Severity::Blocker, QStringLiteral("prefix-conflict"),
                 QStringLiteral("A different Proton prefix already exists at the destination"),
                 QStringLiteral("Source: %1\nDestination: %2\n\nBack it up or remove it manually; "
                                "the mover will not overwrite or merge Proton prefixes.")
                     .arg(resolvedSourcePrefix, resolvedDestPrefix));
    } else if (sourcePrefixExists) {
        FileTree prefixTree = FileTree::scan(resolvedSourcePrefix);
        if (!prefixTree.ok) {
            addIssue(plan, Severity::Blocker, QStringLiteral("scan-prefix-failed"),
                     QStringLiteral("Cannot scan the Proton prefix"), prefixTree.error);
        } else {
            if (destinationExists(resolvedDestPrefix)) {
                addIssue(plan, Severity::Blocker, QStringLiteral("destination-prefix-exists"),
                         QStringLiteral("The destination prefix path exists"), resolvedDestPrefix);
            }
            if (dest->compatdata.target.isEmpty()
                && dest->compatdata.state == CompatDataState::Symlink) {
                addIssue(plan, Severity::Blocker, QStringLiteral("compatdata-broken"),
                         QStringLiteral("The destination compatdata symlink has no target"),
                         dest->compatdata.path);
            }
            if (dest->mount.isWindowsCompatible())
                validateTreeForWindows(prefixTree, dest->mount.fsClass == FsClass::Fat32, &plan.issues);

            Component prefixComponent;
            prefixComponent.kind = ComponentKind::Prefix;
            prefixComponent.source = resolvedSourcePrefix;
            prefixComponent.destination = resolvedDestPrefix;
            prefixComponent.bytes = prefixTree.uniqueBytes;
            plan.components.append(prefixComponent);
            addBytesForMount(plan, mounts, resolvedDestPrefix, prefixComponent.bytes);
        }
    } else if (!prefixError.isEmpty()) {
        addIssue(plan, Severity::Warning, QStringLiteral("prefix-unknown"),
                 QStringLiteral("Could not inspect the source Proton prefix"), prefixError);
    } else {
        addIssue(plan, Severity::Warning, QStringLiteral("no-prefix"),
                 QStringLiteral("No Proton prefix was found"),
                 QStringLiteral("The game may not have been launched yet. This is not a problem."));
    }
    if (!destPrefixError.isEmpty()) {
        addIssue(plan, Severity::Warning, QStringLiteral("dest-prefix-unknown"),
                 QStringLiteral("Could not inspect the destination Proton prefix"), destPrefixError);
    }

    if (options.moveShaderCache) {
        const QString shaderSource = joinPath(joinPath(source->steamappsPath(),
                                                       QStringLiteral("shadercache")),
                                              QString::number(appId));
        const QString shaderDest = joinPath(joinPath(dest->steamappsPath(),
                                                     QStringLiteral("shadercache")),
                                            QString::number(appId));
        if (pathExistsNoFollow(shaderSource)) {
            addTreeComponent(plan, mounts, ComponentKind::ShaderCache, shaderSource, shaderDest,
                             false, dest->mount.isWindowsCompatible(),
                             dest->mount.fsClass == FsClass::Fat32);
        }
    } else if (pathExistsNoFollow(joinPath(joinPath(source->steamappsPath(),
                                                   QStringLiteral("shadercache")),
                                          QString::number(appId)))) {
        addIssue(plan, Severity::Warning, QStringLiteral("shadercache-skipped"),
                 QStringLiteral("Shader cache will be left behind"),
                 QStringLiteral("Steam will regenerate it on the destination."));
    }

    if (options.moveWorkshop) {
        const QString workshopSource =
            joinPath(joinPath(source->steamappsPath(), QStringLiteral("workshop/content")),
                     QString::number(appId));
        const QString workshopDest =
            joinPath(joinPath(dest->steamappsPath(), QStringLiteral("workshop/content")),
                     QString::number(appId));
        if (pathExistsNoFollow(workshopSource)) {
            addTreeComponent(plan, mounts, ComponentKind::WorkshopContent, workshopSource,
                             workshopDest, false, dest->mount.isWindowsCompatible(),
                             dest->mount.fsClass == FsClass::Fat32);
        }
        const QString workshopManifestSource =
            joinPath(joinPath(source->steamappsPath(), QStringLiteral("workshop")),
                     QStringLiteral("appworkshop_%1.acf").arg(appId));
        const QString workshopManifestDest =
            joinPath(joinPath(dest->steamappsPath(), QStringLiteral("workshop")),
                     QStringLiteral("appworkshop_%1.acf").arg(appId));
        if (pathExistsNoFollow(workshopManifestSource)) {
            addFileComponent(plan, mounts, ComponentKind::WorkshopManifest, workshopManifestSource,
                             workshopManifestDest, false);
        }
    } else {
        const QString workshopSource =
            joinPath(joinPath(source->steamappsPath(), QStringLiteral("workshop/content")),
                     QString::number(appId));
        if (pathExistsNoFollow(workshopSource)) {
            addIssue(plan, Severity::Warning, QStringLiteral("workshop-skipped"),
                     QStringLiteral("Workshop content will be left behind"),
                     QStringLiteral("Steam may need to re-download the workshop items at the destination."));
        }
    }

    // Manifest is copied last by the executor so Steam never sees an incomplete install.
    const QString manifestDest =
        joinPath(dest->steamappsPath(),
                 QStringLiteral("appmanifest_%1.acf").arg(appId));
    addFileComponent(plan, mounts, ComponentKind::Manifest, game->manifestPath, manifestDest, true);

    if (options.keepSourceBackup) {
        addIssue(plan, Severity::Warning, QStringLiteral("source-backup"),
                 QStringLiteral("Source payload will be kept as a hidden backup"),
                 QStringLiteral("The game is registered at the destination, but the source folder "
                                "and prefix are renamed to .slm-backup-* and not deleted. Remove "
                                "them manually when you no longer need the backup."));
    }

    // Free-space check on every affected filesystem, including bytes already
    // reserved by earlier games in a batch queue.
    for (auto it = plan.bytesPerMount.constBegin(); it != plan.bytesPerMount.constEnd(); ++it) {
        const quint64 needed = it.value() + reserveFor(it.value());
        const quint64 rawFree = plan.freePerMount.value(it.key(), 0);
        const quint64 reserved = reservedBytes.value(it.key(), 0);
        const quint64 free = rawFree > reserved ? rawFree - reserved : 0;
        plan.requiredPerMount.insert(it.key(), needed);
        if (needed > free) {
            addIssue(plan, Severity::Blocker, QStringLiteral("insufficient-space"),
                     QStringLiteral("Not enough free space on %1").arg(it.key()),
                     QStringLiteral("Need %1 including reserve; %2 free%3.")
                         .arg(formatBytes(needed), formatBytes(free),
                              reserved > 0
                                  ? QStringLiteral(" (%1 reserved by earlier games in this queue)")
                                        .arg(formatBytes(reserved))
                                  : QString()));
        }
    }

    const QFileInfo manifestInfo(game->manifestPath);
    const QFileInfo vdfInfo(scan.libraryFoldersPath);
    QString hashError;
    plan.snapshot.manifestHash = sha256File(game->manifestPath, &hashError);
    if (plan.snapshot.manifestHash.isEmpty())
        addIssue(plan, Severity::Blocker, QStringLiteral("manifest-hash"), QStringLiteral("Cannot hash manifest"), hashError);
    plan.snapshot.manifestMtime = manifestInfo.lastModified().toMSecsSinceEpoch();
    plan.snapshot.manifestSize = manifestInfo.size();
    plan.snapshot.vdfHash = QCryptographicHash::hash(scan.libraryFoldersRaw, QCryptographicHash::Sha256);
    plan.snapshot.vdfMtime = vdfInfo.lastModified().toMSecsSinceEpoch();
    plan.snapshot.vdfSize = vdfInfo.size();
    plan.snapshot.sourceMountId = QString::number(source->mount.mountId);
    plan.snapshot.destinationMountId = QString::number(dest->mount.mountId);
    QString prefixCheckPath;
    if (dest->compatdata.state == CompatDataState::Symlink)
        prefixCheckPath = dest->compatdata.target;
    else if (dest->compatdata.state == CompatDataState::Directory)
        prefixCheckPath = dest->compatdata.path;
    else
        prefixCheckPath = dest->steamappsPath();
    plan.snapshot.prefixCheckPath = prefixCheckPath;
    plan.snapshot.prefixMountId =
        QString::number(mounts.forPath(prefixCheckPath).mountId);
    plan.snapshot.sourceDevice = deviceFor(source->path);
    plan.snapshot.destinationDevice = deviceFor(dest->path);
    plan.snapshot.prefixDevice = deviceFor(prefixCheckPath);

    const QStringList prefixes = processPrefixPaths(scan, *game, *dest);
    const ProcessUsage usage = inspectProcesses(prefixes,
                                                 QStringList{sourceGame});
    if (usage.steamRunning) {
        addIssue(plan, Severity::Blocker, QStringLiteral("steam-running"),
                 QStringLiteral("Steam is running"),
                 QStringLiteral("Close Steam completely before moving games. Quit it, then rescan."));
    }
    if (usage.prefixInUse) {
        addIssue(plan, Severity::Blocker, QStringLiteral("prefix-in-use"),
                 QStringLiteral("A Proton/Wine process is using the prefix"),
                 usage.prefixUsers.join(QStringLiteral(", ")));
    }
    if (usage.gameInUse) {
        addIssue(plan, Severity::Blocker, QStringLiteral("game-in-use"),
                 QStringLiteral("A process is using the game directory"),
                 usage.gameUsers.join(QStringLiteral(", ")));
    }

    for (const Component &component : plan.components)
        plan.totalBytes += component.bytes;

    if (error)
        error->clear();
    return plan;
}

bool MovePlanner::refreshVdfSnapshot(MovePlan &plan, QString *error)
{
    QFileInfo info(plan.libraryFoldersPath);
    if (!info.exists() || !info.isFile()) {
        if (error)
            *error = QStringLiteral("libraryfolders.vdf is no longer available");
        return false;
    }
    QString hashError;
    const QByteArray hash = sha256File(plan.libraryFoldersPath, &hashError);
    if (hash.isEmpty()) {
        if (error)
            *error = hashError;
        return false;
    }
    plan.snapshot.vdfHash = hash;
    plan.snapshot.vdfMtime = info.lastModified().toMSecsSinceEpoch();
    plan.snapshot.vdfSize = info.size();
    if (error)
        error->clear();
    return true;
}

bool MovePlanner::revalidate(const MovePlan &plan, const MountTable &mounts, QString *error)
{
    const auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    const auto checkDevice = [&](const QString &path, quint64 expected, const QString &label) -> bool {
        struct stat st {};
        if (path.isEmpty() || ::stat(QFile::encodeName(path).constData(), &st) != 0)
            return fail(QStringLiteral("%1 is no longer available: %2").arg(label, path)), false;
        if (expected != 0 && static_cast<quint64>(st.st_dev) != expected)
            return fail(QStringLiteral("%1 changed filesystem (was the drive unmounted?): %2")
                            .arg(label, path)), false;
        return true;
    };
    if (!checkDevice(plan.sourceLibraryPath, plan.snapshot.sourceDevice, QStringLiteral("Source library")))
        return false;
    if (!checkDevice(plan.destLibraryPath, plan.snapshot.destinationDevice,
                     QStringLiteral("Destination library")))
        return false;
    if (plan.snapshot.prefixDevice != 0
        && !checkDevice(plan.snapshot.prefixCheckPath, plan.snapshot.prefixDevice,
                        QStringLiteral("Proton prefix filesystem")))
        return false;

    QFileInfo manifestInfo(plan.game.manifestPath);
    if (!manifestInfo.exists())
        return fail(QStringLiteral("The source app manifest disappeared"));
    if (manifestInfo.size() != plan.snapshot.manifestSize)
        return fail(QStringLiteral("The source app manifest changed size"));
    const QByteArray hash = sha256File(plan.game.manifestPath);
    if (hash != plan.snapshot.manifestHash)
        return fail(QStringLiteral("The source app manifest changed"));

    const QString rootVdf = plan.libraryFoldersPath;
    QFile file(rootVdf);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Cannot reopen libraryfolders.vdf"));
    const QByteArray raw = file.readAll();
    if (QCryptographicHash::hash(raw, QCryptographicHash::Sha256) != plan.snapshot.vdfHash)
        return fail(QStringLiteral("libraryfolders.vdf changed while planning"));

    const QStringList prefixes = [&]() {
        QStringList result;
        for (const Component &component : plan.components) {
            if (component.kind == ComponentKind::Prefix) {
                result << component.source << component.destination;
            }
        }
        result.removeDuplicates();
        return result;
    }();
    const ProcessUsage usage = inspectProcesses(prefixes, QStringList{plan.components.isEmpty()
                                                                          ? QString()
                                                                          : plan.components.first().source});
    if (usage.steamRunning)
        return fail(QStringLiteral("Steam started while planning"));
    if (usage.prefixInUse)
        return fail(QStringLiteral("A Proton/Wine process started using the prefix"));
    if (usage.gameInUse)
        return fail(QStringLiteral("A process started using the game directory"));

    const MountInfo currentSource = mounts.forPath(plan.sourceLibraryPath);
    const MountInfo currentDest = mounts.forPath(plan.destLibraryPath);
    if (currentSource.readOnly || currentDest.readOnly)
        return fail(QStringLiteral("A source or destination library is now mounted read-only"));
    if (currentSource.isNetwork() || currentDest.isNetwork())
        return fail(QStringLiteral("A source or destination library is now on a network filesystem"));
    if (currentDest.freeBytes > 0 && currentDest.freeBytes < plan.totalBytes)
        return fail(QStringLiteral("The destination no longer has enough free space"));
    return true;
}

} // namespace slm
