// SPDX-License-Identifier: GPL-3.0-or-later
#include "health.h"

#include "fileops.h"
#include "library.h"
#include "process_check.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <cerrno>
#include <cstring>

#include <unistd.h>

namespace slm {

namespace {

QString joinPath(const QString &base, const QString &name)
{
    if (base.endsWith(QLatin1Char('/')))
        return base + name;
    return base + QLatin1Char('/') + name;
}

bool isNumericDirectoryName(const QString &name)
{
    if (name.isEmpty())
        return false;
    for (const QChar c : name) {
        if (!c.isDigit())
            return false;
    }
    return true;
}

bool looksLikeSourceBackup(const QString &name)
{
    return name.startsWith(QStringLiteral(".slm-backup-"));
}

bool isKnownSharedPayload(const QString &name)
{
    return name == QStringLiteral("Steamworks Shared")
        || name == QStringLiteral("Steam Controller Configs")
        || name == QStringLiteral("SteamVR")
        || name.startsWith(QStringLiteral("SteamLinuxRuntime"), Qt::CaseInsensitive);
}

QString displayLibrary(const SteamScan &scan, const QString &path)
{
    const Library *library = scan.findLibrary(path);
    return library ? library->rawPath : path;
}

void add(QVector<HealthFinding> *findings, HealthSeverity severity, HealthKind kind,
         const SteamScan &scan, const QString &libraryPath, const QString &path,
         const QString &title, const QString &detail, bool quarantineable = false)
{
    HealthFinding finding;
    finding.severity = severity;
    finding.kind = kind;
    finding.libraryPath = displayLibrary(scan, libraryPath);
    finding.path = path;
    finding.title = title;
    finding.detail = detail;
    finding.quarantineable = quarantineable;
    findings->append(finding);
}

void scanLocksInDirectory(QVector<HealthFinding> *findings, const SteamScan &scan,
                          const QString &libraryPath, const QString &directory,
                          const QString &context)
{
    QDir dir(directory);
    if (!dir.exists())
        return;
    const QFileInfoList locks =
        dir.entryInfoList({QStringLiteral("*.lock")}, QDir::Files | QDir::Hidden, QDir::Name);
    for (const QFileInfo &lock : locks) {
        add(findings, HealthSeverity::Warning, HealthKind::StaleLockFile, scan, libraryPath,
            lock.absoluteFilePath(),
            QStringLiteral("Stale-looking lock file"),
            QStringLiteral("%1. If Steam and all games are closed, it can be moved to "
                           "a hidden .slm-quarantine folder. It is never deleted.")
                .arg(context),
            true);
    }
}

} // namespace

QString LibraryHealth::kindName(HealthKind kind)
{
    switch (kind) {
    case HealthKind::MissingGamePayload:
        return QStringLiteral("Missing game payload");
    case HealthKind::OrphanGameFolder:
        return QStringLiteral("Unmanaged game folder");
    case HealthKind::DuplicateAppId:
        return QStringLiteral("Duplicate appid");
    case HealthKind::StaleTempManifest:
        return QStringLiteral("Stale manifest temp file");
    case HealthKind::StaleLockFile:
        return QStringLiteral("Lock file");
    case HealthKind::BrokenCompatData:
        return QStringLiteral("Broken compatdata");
    case HealthKind::CompatDataOnWindowsFs:
        return QStringLiteral("compatdata on Windows FS");
    case HealthKind::OrphanShaderCache:
        return QStringLiteral("Orphan shader cache");
    case HealthKind::OrphanWorkshopContent:
        return QStringLiteral("Orphan Workshop content");
    case HealthKind::OrphanPrefix:
        return QStringLiteral("Orphan Proton prefix");
    case HealthKind::SourceBackup:
        return QStringLiteral("Source backup");
    }
    return QStringLiteral("Finding");
}

QString LibraryHealth::severityName(HealthSeverity severity)
{
    switch (severity) {
    case HealthSeverity::Info:
        return QStringLiteral("Info");
    case HealthSeverity::Warning:
        return QStringLiteral("Warning");
    case HealthSeverity::Critical:
        return QStringLiteral("Critical");
    }
    return QStringLiteral("Info");
}

QVector<HealthFinding> LibraryHealth::scan(const SteamScan &scan, QString *error)
{
    QVector<HealthFinding> findings;
    QSet<quint32> allAppIds;
    for (const Game &game : scan.games) {
        if (game.appId != 0)
            allAppIds.insert(game.appId);
    }

    // Duplicate appids are visible across manifests but ambiguous to Steam.
    QMap<quint32, int> appCounts;
    for (const Game &game : scan.games) {
        if (game.appId != 0)
            appCounts[game.appId]++;
    }
    for (const Game &game : scan.games) {
        if (game.appId != 0 && appCounts.value(game.appId) > 1) {
            add(&findings, HealthSeverity::Warning, HealthKind::DuplicateAppId, scan,
                game.libraryPath, game.manifestPath,
                QStringLiteral("App %1 is installed in multiple libraries").arg(game.appId),
                QStringLiteral("Resolve the duplicate before moving this app."));
        }
        for (const Issue &issue : game.issues) {
            if (issue.code == QStringLiteral("payload-missing")) {
                add(&findings, HealthSeverity::Critical, HealthKind::MissingGamePayload, scan,
                    game.libraryPath,
                    joinPath(joinPath(game.libraryPath, QStringLiteral("steamapps/common")),
                             game.installDir),
                    QStringLiteral("Manifest exists but payload is missing"),
                    QStringLiteral("%1: %2").arg(game.name, issue.message));
            }
        }
    }

    QSet<QString> scannedCompatRoots;
    for (const Library &library : scan.libraries) {
        const QString steamapps = library.steamappsPath();
        if (!QFileInfo::exists(steamapps)) {
            add(&findings, HealthSeverity::Critical, HealthKind::MissingGamePayload, scan,
                library.path, steamapps, QStringLiteral("Steam library is not available"),
                QStringLiteral("The steamapps directory is missing."));
            continue;
        }

        if (library.compatdata.state == CompatDataState::BrokenSymlink) {
            add(&findings, HealthSeverity::Critical, HealthKind::BrokenCompatData, scan,
                library.path, library.compatdataPath(),
                QStringLiteral("compatdata symlink is broken"), library.compatdata.error);
        } else if (library.compatdata.targetOnWindowsFs) {
            add(&findings, HealthSeverity::Warning, HealthKind::CompatDataOnWindowsFs, scan,
                library.path, library.compatdata.target,
                QStringLiteral("Proton prefix storage is on a Windows filesystem"),
                QStringLiteral("Move prefixes to a Linux filesystem and repair the compatdata symlink."));
        }

        const QDir steamappsDir(steamapps);
        const QFileInfoList tempManifests = steamappsDir.entryInfoList(
            {QStringLiteral("appmanifest_*.acf.*.tmp"), QStringLiteral("appmanifest_*.tmp")},
            QDir::Files | QDir::Hidden, QDir::Name);
        for (const QFileInfo &temp : tempManifests) {
            add(&findings, HealthSeverity::Warning, HealthKind::StaleTempManifest, scan,
                library.path, temp.absoluteFilePath(),
                QStringLiteral("Stale manifest temporary file"),
                QStringLiteral("Steam does not use this temporary manifest. It can be moved to a "
                               "hidden .slm-quarantine folder."),
                true);
        }
        scanLocksInDirectory(&findings, scan, library.path, steamapps,
                             QStringLiteral("Library-level lock file"));

        QSet<QString> installDirs;
        for (const Game &game : scan.games) {
            if (canonicalOrAbsolute(game.libraryPath) == canonicalOrAbsolute(library.path)
                && !game.installDir.isEmpty())
                installDirs.insert(game.installDir);
        }

        const QDir commonDir(joinPath(steamapps, QStringLiteral("common")));
        if (commonDir.exists()) {
            const QFileInfoList entries =
                commonDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden,
                                        QDir::Name);
            for (const QFileInfo &entry : entries) {
                const QString name = entry.fileName();
                if (looksLikeSourceBackup(name)) {
                    add(&findings, HealthSeverity::Info, HealthKind::SourceBackup, scan,
                        library.path, entry.absoluteFilePath(),
                        QStringLiteral("Source backup"),
                        QStringLiteral("Created by keep-source-backup mode. Remove manually when "
                                       "the destination copy is trusted."));
                    continue;
                }
                if (entry.fileName().startsWith(QLatin1Char('.')))
                    continue;
                if (installDirs.contains(name) || isKnownSharedPayload(name))
                    continue;
                add(&findings, HealthSeverity::Warning, HealthKind::OrphanGameFolder, scan,
                    library.path, entry.absoluteFilePath(),
                    QStringLiteral("Game folder has no manifest"),
                    QStringLiteral("Steam will not manage this folder. It is not moved automatically."));
            }
            for (const Game &game : scan.games) {
                if (canonicalOrAbsolute(game.libraryPath) != canonicalOrAbsolute(library.path))
                    continue;
                if (game.category != GameCategory::Game)
                    continue;
                const QString payload =
                    joinPath(joinPath(steamapps, QStringLiteral("common")), game.installDir);
                scanLocksInDirectory(&findings, scan, library.path, payload,
                                     QStringLiteral("Game folder lock file"));
                bool prefixExists = false;
                const QString prefix = resolvePrefixPath(library, game.appId, &prefixExists);
                if (prefixExists) {
                    scanLocksInDirectory(&findings, scan, library.path, prefix,
                                         QStringLiteral("Proton prefix lock file"));
                }
            }
        }

        const QDir shaderDir(joinPath(steamapps, QStringLiteral("shadercache")));
        if (shaderDir.exists()) {
            for (const QFileInfo &entry :
                 shaderDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden,
                                         QDir::Name)) {
                if (looksLikeSourceBackup(entry.fileName())) {
                    add(&findings, HealthSeverity::Info, HealthKind::SourceBackup, scan,
                        library.path, entry.absoluteFilePath(), QStringLiteral("Source backup"),
                        QStringLiteral("Created by keep-source-backup mode."));
                } else if (isNumericDirectoryName(entry.fileName())
                           && entry.fileName() != QStringLiteral("0")
                           && !allAppIds.contains(entry.fileName().toUInt())) {
                    add(&findings, HealthSeverity::Info, HealthKind::OrphanShaderCache, scan,
                        library.path, entry.absoluteFilePath(),
                        QStringLiteral("Shader cache has no installed app"),
                        QStringLiteral("Likely left behind by an uninstalled game."));
                }
            }
        }

        const QString workshopRoot = joinPath(steamapps, QStringLiteral("workshop"));
        scanLocksInDirectory(&findings, scan, library.path, workshopRoot,
                             QStringLiteral("Workshop-level lock file"));
        const QDir workshopContent(joinPath(workshopRoot, QStringLiteral("content")));
        if (workshopContent.exists()) {
            for (const QFileInfo &entry :
                 workshopContent.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden,
                                               QDir::Name)) {
                if (looksLikeSourceBackup(entry.fileName())) {
                    add(&findings, HealthSeverity::Info, HealthKind::SourceBackup, scan,
                        library.path, entry.absoluteFilePath(), QStringLiteral("Source backup"),
                        QStringLiteral("Created by keep-source-backup mode."));
                } else if (isNumericDirectoryName(entry.fileName())
                           && entry.fileName() != QStringLiteral("0")
                           && !allAppIds.contains(entry.fileName().toUInt())) {
                    add(&findings, HealthSeverity::Info, HealthKind::OrphanWorkshopContent, scan,
                        library.path, entry.absoluteFilePath(),
                        QStringLiteral("Workshop content has no installed app"),
                        QStringLiteral("Likely left behind by an uninstalled game."));
                }
            }
        }

        QString compatRoot;
        if (library.compatdata.state == CompatDataState::Symlink)
            compatRoot = library.compatdata.target;
        else if (library.compatdata.state == CompatDataState::Directory)
            compatRoot = library.compatdata.path;
        if (!compatRoot.isEmpty() && !scannedCompatRoots.contains(compatRoot)) {
            scannedCompatRoots.insert(compatRoot);
            QDir compatDir(compatRoot);
            if (compatDir.exists()) {
                for (const QFileInfo &entry :
                     compatDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden,
                                             QDir::Name)) {
                    if (looksLikeSourceBackup(entry.fileName())) {
                        add(&findings, HealthSeverity::Info, HealthKind::SourceBackup, scan,
                            library.path, entry.absoluteFilePath(), QStringLiteral("Source backup"),
                            QStringLiteral("Created by keep-source-backup mode."));
                    } else if (isNumericDirectoryName(entry.fileName())
                               && entry.fileName() != QStringLiteral("0")
                               && !allAppIds.contains(entry.fileName().toUInt())) {
                        add(&findings, HealthSeverity::Info, HealthKind::OrphanPrefix, scan,
                            library.path, entry.absoluteFilePath(),
                            QStringLiteral("Proton prefix has no installed app"),
                            QStringLiteral("Likely left behind by an uninstalled game."));
                    }
                }
            }
        }
    }

    if (error)
        error->clear();
    return findings;
}

bool LibraryHealth::quarantine(const SteamScan &scan, const QVector<HealthFinding> &findings,
                               QString *error)
{
    if (::geteuid() == 0) {
        if (error)
            *error = QStringLiteral("Refusing to quarantine files as root.");
        return false;
    }
    QStringList prefixPaths;
    QStringList gameDirs;
    for (const Game &game : scan.games) {
        const Library *library = scan.findLibrary(game.libraryPath);
        if (!library)
            continue;
        bool prefixExists = false;
        const QString prefix = resolvePrefixPath(*library, game.appId, &prefixExists);
        if (prefixExists)
            prefixPaths.append(prefix);
        if (!game.installDir.isEmpty()) {
            gameDirs.append(joinPath(
                joinPath(library->steamappsPath(), QStringLiteral("common")), game.installDir));
        }
    }
    const ProcessUsage usage = inspectProcesses(prefixPaths, gameDirs);
    if (usage.steamRunning || usage.prefixInUse || usage.gameInUse) {
        if (error)
            *error = QStringLiteral("Close Steam and all Proton/Wine games before quarantining "
                                    "lock files.");
        return false;
    }

    QStringList errors;
    int moved = 0;
    for (const HealthFinding &finding : findings) {
        if (!finding.quarantineable)
            continue;
        const QFileInfo info(finding.path);
        if (!info.exists())
            continue;
        const QString parent = info.absolutePath();
        const QString quarantineDir = joinPath(parent, QStringLiteral(".slm-quarantine"));
        if (!fsutil::ensureDir(quarantineDir, error)) {
            errors << (error ? *error : QStringLiteral("Cannot create %1").arg(quarantineDir));
            continue;
        }
        const QString stamp =
            QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        const QString target = joinPath(
            quarantineDir,
            QStringLiteral("%1.%2-%3")
                .arg(info.fileName(), stamp, fsutil::uniqueSuffix()));
        if (::rename(QFile::encodeName(finding.path).constData(),
                     QFile::encodeName(target).constData())
            != 0) {
            errors << QStringLiteral("Cannot quarantine %1: %2")
                          .arg(finding.path, QString::fromLocal8Bit(::strerror(errno)));
        } else {
            ++moved;
        }
    }
    if (moved == 0 && errors.isEmpty()) {
        if (error)
            *error = QStringLiteral("No quarantinable files were selected.");
        return false;
    }
    if (!errors.isEmpty()) {
        if (error)
            *error = errors.join(QLatin1Char('\n'));
        return false;
    }
    if (error)
        error->clear();
    return true;
}

} // namespace slm
