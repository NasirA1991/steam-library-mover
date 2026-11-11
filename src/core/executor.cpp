// SPDX-License-Identifier: GPL-3.0-or-later
#include "executor.h"

#include "copyengine.h"
#include "fileops.h"
#include "journal.h"
#include "library.h"
#include "mountinfo.h"
#include "planner.h"
#include "process_check.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <cerrno>
#include <cstring>
#include <limits>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace slm {

namespace {

QString joinPath(const QString &base, const QString &name)
{
    if (base.endsWith(QLatin1Char('/')))
        return base + name;
    return base + QLatin1Char('/') + name;
}

QJsonArray componentsToJson(const QVector<Component> &components)
{
    QJsonArray array;
    for (const Component &component : components) {
        QJsonObject object;
        object.insert(QStringLiteral("kind"), static_cast<int>(component.kind));
        object.insert(QStringLiteral("source"), component.source);
        object.insert(QStringLiteral("destination"), component.destination);
        object.insert(QStringLiteral("bytes"), static_cast<qint64>(component.bytes));
        object.insert(QStringLiteral("required"), component.required);
        object.insert(QStringLiteral("skip"), component.skip);
        object.insert(QStringLiteral("note"), component.note);
        array.append(object);
    }
    return array;
}

QString phaseKey(Phase phase)
{
    return phaseName(phase);
}

bool phaseAllowsResume(const QString &phase)
{
    return phase == phaseName(Phase::Planned)
        || phase == phaseName(Phase::CopyingGame)
        || phase == phaseName(Phase::CopyingAux)
        || phase == phaseName(Phase::Verifying)
        || phase == phaseName(Phase::Syncing);
}

QString manifestBackupName(quint32 appId)
{
    return QStringLiteral("appmanifest_%1.acf").arg(appId);
}

bool backupFile(const QString &source, const QString &destination, QString *error)
{
    const auto data = fsutil::readFile(source, 32 * 1024 * 1024, error);
    if (!data)
        return false;
    if (!fsutil::ensureDir(QFileInfo(destination).absolutePath(), error))
        return false;
    return fsutil::writeFileAtomic(destination, *data, 0600, error);
}

bool samePath(const QString &a, const QString &b)
{
    return canonicalOrAbsolute(a) == canonicalOrAbsolute(b);
}

bool isDirectoryNoFollow(const QString &path)
{
    struct stat st {};
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0)
        return false;
    return S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
}

bool removeSourcePayload(const QString &path, quint32 appId, const QString &destination,
                         bool keepBackup, QStringList *backupPaths, QString *error)
{
    if (!pathExistsNoFollow(path))
        return true;
    if (samePath(path, destination)) {
        if (error)
            *error = QStringLiteral("Refusing to delete a source that is also the destination: %1").arg(path);
        return false;
    }

    const QString parent = QFileInfo(path).absolutePath();
    if (keepBackup) {
        const QString backup = joinPath(parent, QStringLiteral(".slm-backup-%1-%2")
                                                   .arg(appId)
                                                   .arg(fsutil::uniqueSuffix()));
        if (::rename(QFile::encodeName(path).constData(), QFile::encodeName(backup).constData()) != 0) {
            if (error)
                *error = QStringLiteral("Could not rename %1 to a backup; source was not deleted: %2")
                             .arg(path, QString::fromLocal8Bit(::strerror(errno)));
            return false;
        }
        if (backupPaths)
            backupPaths->append(backup);
        return true;
    }

    if (isDirectoryNoFollow(path)) {
        const QString trash = joinPath(parent, QStringLiteral(".slm-trash-%1-%2")
                                                  .arg(appId)
                                                  .arg(fsutil::uniqueSuffix()));
        if (::rename(QFile::encodeName(path).constData(), QFile::encodeName(trash).constData()) == 0) {
            return fsutil::removeTree(trash, error);
        }
        if (error)
            *error = QStringLiteral("Could not rename %1 to quarantine; source was not deleted: %2")
                         .arg(path, QString::fromLocal8Bit(::strerror(errno)));
        return false;
    }
    return fsutil::removeTree(path, error);
}

void emitProgress(const MoveExecutor::ProgressFn &callback, Phase phase, const QString &message,
                  const QString &currentFile, quint64 done, quint64 total)
{
    if (!callback)
        return;
    MoveProgress progress;
    progress.phase = phase;
    progress.message = message;
    progress.currentFile = currentFile;
    progress.bytesDone = done;
    progress.bytesTotal = total;
    callback(progress);
}

} // namespace

MoveResult MoveExecutor::execute(const MovePlan &plan, const MountTable &mounts,
                                 const ProgressFn &progress, const CancelFn &cancelled,
                                 const PauseFn &paused, const QJsonObject &resumeJournal)
{
    MoveResult result;
    const bool resuming = !resumeJournal.isEmpty();
    if (::geteuid() == 0) {
        result.error = QStringLiteral("Refusing to move games as root");
        return result;
    }
    if (plan.hasBlocker()) {
        result.error = QStringLiteral("The move plan still has blockers");
        return result;
    }
    if (resuming) {
        QString reason;
        if (!canResume(resumeJournal, &reason)) {
            result.error = QStringLiteral("This operation cannot be resumed: %1").arg(reason);
            return result;
        }
    }
    QString stateError;
    {
        MountTable live = MountTable::load(&stateError);
        if (!live.isValid()) {
            result.error = QStringLiteral("Cannot inspect current mounts: %1").arg(stateError);
            return result;
        }
        if (!MovePlanner::revalidateMountState(plan, live, &stateError)) {
            result.error = QStringLiteral("Preflight failed: %1").arg(stateError);
            return result;
        }
    }
    if (!JournalStore::ensure(&stateError)) {
        result.error = stateError;
        return result;
    }

    const QString opId = resuming ? resumeJournal.value(QStringLiteral("opId")).toString()
                                  : JournalStore::newOperationId();
    const QString journalPath = JournalStore::journalPath(opId);
    const QString backupDir = JournalStore::backupDir(opId);
    if (!resuming) {
        QString backupError;
        if (!fsutil::ensureDir(backupDir, &backupError)) {
            result.error = backupError;
            return result;
        }
    } else if (!QFileInfo(backupDir).isDir()) {
        result.error = QStringLiteral("The operation backup directory is missing");
        return result;
    }

    // Always use the standard log path; never trust an arbitrary path from a journal.
    const QString logPath = JournalStore::logPath(opId);
    Logger logger(logPath);
    if (!logger.open(&stateError)) {
        result.error = stateError;
        return result;
    }
    result.journalPath = journalPath;
    result.logPath = logger.path();
    logger.write(QStringLiteral("Move operation %1 %2 for app %3 (%4)")
                     .arg(opId, resuming ? QStringLiteral("resuming") : QStringLiteral("starting"))
                     .arg(plan.game.appId)
                     .arg(plan.game.name));

    QJsonObject journal;
    if (resuming) {
        journal = resumeJournal;
    } else {
        journal.insert(QStringLiteral("version"), 1);
        journal.insert(QStringLiteral("opId"), opId);
        journal.insert(QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        journal.insert(QStringLiteral("appId"), static_cast<qint64>(plan.game.appId));
        journal.insert(QStringLiteral("name"), plan.game.name);
        journal.insert(QStringLiteral("installDir"), plan.game.installDir);
        journal.insert(QStringLiteral("sourceLibrary"), plan.sourceLibraryPath);
        journal.insert(QStringLiteral("destinationLibrary"), plan.destLibraryPath);
        journal.insert(QStringLiteral("manifest"), plan.game.manifestPath);
        journal.insert(QStringLiteral("vdf"), plan.libraryFoldersPath);
        journal.insert(QStringLiteral("backupDir"), backupDir);
        journal.insert(QStringLiteral("logPath"), logger.path());
        journal.insert(QStringLiteral("components"), componentsToJson(plan.components));
        journal.insert(QStringLiteral("keepSourceBackup"), plan.options.keepSourceBackup);
    }
    journal.insert(QStringLiteral("state"), QStringLiteral("in-progress"));
    journal.insert(QStringLiteral("phase"), phaseKey(Phase::Planned));
    journal.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    journal.insert(QStringLiteral("fullVerify"), plan.options.fullVerify);
    journal.insert(QStringLiteral("error"), QString());
    journal.insert(QStringLiteral("attempts"),
                   journal.value(QStringLiteral("attempts")).toInt() + 1);

    auto writeJournal = [&]() {
        journal.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        QString error;
        if (!JournalStore::writeObject(journalPath, journal, &error)) {
            logger.write(QStringLiteral("ERROR writing journal: %1").arg(error));
            return false;
        }
        return true;
    };
    auto setPhase = [&](Phase phase, const QString &message) {
        journal.insert(QStringLiteral("phase"), phaseKey(phase));
        if (!message.isEmpty())
            journal.insert(QStringLiteral("message"), message);
        writeJournal();
    };
    auto fail = [&](const QString &message) -> MoveResult {
        journal.insert(QStringLiteral("state"), QStringLiteral("failed"));
        journal.insert(QStringLiteral("error"), message);
        writeJournal();
        logger.write(QStringLiteral("FAILED: %1").arg(message));
        result.ok = false;
        result.error = message;
        return result;
    };

    if (!writeJournal())
        return fail(QStringLiteral("Cannot write operation journal"));

    auto notify = [&](Phase phase, const QString &message, const QString &current, quint64 done) {
        emitProgress(progress, phase, message, current, done, plan.totalBytes);
        logger.write(QStringLiteral("%1: %2%3")
                         .arg(phaseName(phase), message,
                              current.isEmpty() ? QString() : QStringLiteral(" [%1]").arg(current)));
    };
    auto cancelMove = [&]() -> MoveResult {
        journal.insert(QStringLiteral("state"), QStringLiteral("cancelled"));
        writeJournal();
        logger.write(QStringLiteral("CANCELLED by user"));
        result.cancelled = true;
        result.error = QStringLiteral("Cancelled by user");
        emitProgress(progress, Phase::Cancelled, result.error, {}, plan.totalBytes,
                     plan.totalBytes);
        return result;
    };

    setPhase(Phase::Planned, QStringLiteral("Revalidating"));
    if (!MovePlanner::revalidate(plan, mounts, &stateError))
        return fail(QStringLiteral("Preflight failed: %1").arg(stateError));

    if (!resuming) {
        if (!backupFile(plan.libraryFoldersPath,
                        joinPath(backupDir, QStringLiteral("libraryfolders.vdf")), &stateError)) {
            return fail(QStringLiteral("Cannot back up libraryfolders.vdf: %1").arg(stateError));
        }
        if (!backupFile(plan.game.manifestPath,
                        joinPath(backupDir, manifestBackupName(plan.game.appId)), &stateError)) {
            return fail(QStringLiteral("Cannot back up appmanifest: %1").arg(stateError));
        }
    }

    QStringList monitorPrefixes;
    QStringList monitorGameDirs;
    for (const Component &component : plan.components) {
        if (component.skip)
            continue;
        if (component.kind == ComponentKind::Prefix) {
            if (!component.source.isEmpty())
                monitorPrefixes.append(component.source);
            if (!component.destination.isEmpty())
                monitorPrefixes.append(component.destination);
        } else if (component.kind == ComponentKind::Game) {
            if (!component.source.isEmpty())
                monitorGameDirs.append(component.source);
            if (!component.destination.isEmpty())
                monitorGameDirs.append(component.destination);
        }
    }
    monitorPrefixes.removeDuplicates();
    monitorGameDirs.removeDuplicates();

    // Steam or a game may start after the initial preflight. Poll while copying
    // and verifying; a mid-copy process must stop the operation before commit.
    QElapsedTimer processClock;
    processClock.start();
    QString monitorError;
    bool monitorTriggered = false;
    const auto checkProcesses = [&]() -> bool {
        if (monitorTriggered)
            return false;
        if (processClock.elapsed() < 30000)
            return true;
        processClock.restart();
        const ProcessUsage usage = inspectProcesses(monitorPrefixes, monitorGameDirs);
        if (usage.steamRunning) {
            monitorError = QStringLiteral("Steam started while the move was running. "
                                          "Stop Steam and resume the operation.");
        } else if (usage.prefixInUse) {
            monitorError = QStringLiteral("A Proton/Wine process started using the prefix. "
                                          "Stop it and resume the operation.");
        } else if (usage.gameInUse) {
            monitorError = QStringLiteral("A game process started using the game directory. "
                                          "Stop it and resume the operation.");
        } else {
            return true;
        }
        monitorTriggered = true;
        logger.write(QStringLiteral("MID-MOVE BLOCKER: %1").arg(monitorError));
        return false;
    };
    const auto abortRequested = [&]() -> bool {
        if (monitorTriggered)
            return true;
        if (cancelled && cancelled())
            return true;
        checkProcesses();
        return monitorTriggered;
    };
    const auto abortResult = [&]() -> MoveResult {
        if (monitorTriggered)
            return fail(monitorError);
        return cancelMove();
    };

    quint64 offset = 0;
    for (const Component &component : plan.components) {
        if (component.kind == ComponentKind::Manifest)
            continue;
        if (component.skip) {
            offset += component.bytes;
            continue;
        }
        if (abortRequested())
            return abortResult();

        const Phase phase = component.kind == ComponentKind::Game ? Phase::CopyingGame
                                                                   : Phase::CopyingAux;
        setPhase(phase, QStringLiteral("Copying %1").arg(componentKindName(component.kind)));
        notify(phase, QStringLiteral("Copying %1").arg(componentKindName(component.kind)), QString(), offset);

        if (!fsutil::ensureDir(QFileInfo(component.destination).absolutePath(), &stateError))
            return fail(stateError);

        // Verification hashes the destination only when copyTree supplied a
        // trusted source digest; fallback verification reads both sides but
        // still reports destination bytes so the bar stays monotonic.
        const quint64 verifyTotal = component.bytes;
        CopyEngine::HashLookup copyHashes;
        const QString verifyMessage = QStringLiteral("Verifying %1%2")
                                          .arg(componentKindName(component.kind),
                                               plan.options.fullVerify
                                                   ? QStringLiteral(" (full checksum)")
                                                   : QString());
        auto verifyComponent = [&](QString *verifyError) -> bool {
            if (component.kind == ComponentKind::WorkshopManifest) {
                const auto sourceData = fsutil::readFile(component.source, 32 * 1024 * 1024, verifyError);
                if (!sourceData)
                    return false;
                const auto destData = fsutil::readFile(component.destination, 32 * 1024 * 1024, verifyError);
                if (!destData)
                    return false;
                if (*sourceData != *destData) {
                    if (verifyError)
                        *verifyError = QStringLiteral("Workshop manifest content differs");
                    return false;
                }
                if (plan.options.fullVerify
                    && QCryptographicHash::hash(*sourceData, QCryptographicHash::Sha256)
                        != QCryptographicHash::hash(*destData, QCryptographicHash::Sha256)) {
                    if (verifyError)
                        *verifyError = QStringLiteral("Workshop manifest checksum differs");
                    return false;
                }
                return true;
            }
            return CopyEngine::verifyTree(
                component.source, component.destination, plan.options.fullVerify, verifyError,
                abortRequested,
                [&](quint64 bytesVerified, const QString &currentFile) {
                    emitProgress(progress, Phase::Verifying, verifyMessage, currentFile,
                                 bytesVerified, verifyTotal);
                },
                plan.options.fullVerify ? &copyHashes : nullptr);
        };
        auto copyComponent = [&](QString *copyError) -> bool {
            if (component.kind == ComponentKind::WorkshopManifest) {
                const auto data = fsutil::readFile(component.source, 32 * 1024 * 1024, copyError);
                if (!data)
                    return false;
                return fsutil::writeFileAtomic(component.destination, *data, 0644, copyError);
            }
            qint64 copied = 0;
            return CopyEngine::copyTree(
                component.source, component.destination,
                [&](quint64 bytesInTree, const QString &currentFile) {
                    emitProgress(progress, phase,
                                 QStringLiteral("Copying %1").arg(componentKindName(component.kind)),
                                 currentFile, offset + bytesInTree, plan.totalBytes);
                },
                abortRequested, &copied, copyError, paused, &copyHashes);
        };

        if (pathExistsNoFollow(component.destination)) {
            if (!resuming) {
                return fail(QStringLiteral("Destination already exists: %1. Remove the partial copy or "
                                           "use recovery before retrying.")
                                .arg(component.destination));
            }
            QString verifyError;
            if (verifyComponent(&verifyError)) {
                logger.write(QStringLiteral("Resume: reusing verified %1 at %2")
                                 .arg(componentKindName(component.kind), component.destination));
                offset += component.bytes;
                notify(phase, QStringLiteral("Reused verified %1")
                                  .arg(componentKindName(component.kind)),
                       component.destination, offset);
                continue;
            }
            if (abortRequested())
                return abortResult();
            logger.write(QStringLiteral("Resume: existing %1 failed verification (%2); recopying")
                             .arg(componentKindName(component.kind), verifyError));
            if (!fsutil::removeTree(component.destination, &stateError))
                return fail(QStringLiteral("Cannot remove incomplete destination: %1").arg(stateError));
        }

        if (!copyComponent(&stateError)) {
            if (abortRequested())
                return abortResult();
            return fail(QStringLiteral("Copy failed: %1").arg(stateError));
        }

        setPhase(Phase::Verifying, verifyMessage);
        // Until hashing reports its first chunk, keep verification visually
        // indeterminate. Reusing the copy offset here would make the bar jump
        // backwards when hashing starts.
        emitProgress(progress, Phase::Verifying, verifyMessage, QString(), 0, 0);
        logger.write(QStringLiteral("Verifying: %1").arg(verifyMessage));
        if (!verifyComponent(&stateError)) {
            if (abortRequested())
                return abortResult();
            // A partial write or transient I/O error can leave a complete-looking
            // copy. Remove it and retry once; resume relies on this to repair a
            // destination that failed verification in an earlier attempt.
            logger.write(QStringLiteral("Verification failed for %1: %2; recopying once")
                             .arg(component.destination, stateError));
            if (!fsutil::removeTree(component.destination, &stateError))
                return fail(QStringLiteral("Cannot remove failed destination: %1").arg(stateError));
            setPhase(phase, QStringLiteral("Repairing %1").arg(componentKindName(component.kind)));
            notify(phase, QStringLiteral("Repairing %1").arg(componentKindName(component.kind)),
                   QString(), offset);
            if (!copyComponent(&stateError)) {
                if (abortRequested())
                    return abortResult();
                return fail(QStringLiteral("Copy failed during repair: %1").arg(stateError));
            }
            if (!verifyComponent(&stateError)) {
                if (abortRequested())
                    return abortResult();
                return fail(QStringLiteral("Verification failed after repair: %1").arg(stateError));
            }
        }
        if (cancelled && cancelled())
            return cancelMove();

        offset += component.bytes;
        notify(phase, QStringLiteral("Copied %1").arg(componentKindName(component.kind)),
               QString(), offset);
    }

    if (cancelled && cancelled())
        return cancelMove();

    // Make the copied data durable before creating the manifest.
    setPhase(Phase::Syncing, QStringLiteral("Syncing destination filesystems"));
    QSet<QString> syncedMounts;
    for (const Component &component : plan.components) {
        if (component.skip || component.destination.isEmpty())
            continue;
        const QString checkPath = isDirectoryNoFollow(component.destination)
            ? component.destination
            : QFileInfo(component.destination).absolutePath();
        const QString mountPoint = mounts.forPath(checkPath).mountPoint;
        if (mountPoint.isEmpty() || syncedMounts.contains(mountPoint))
            continue;
        syncedMounts.insert(mountPoint);
        if (!fsutil::syncFilesystem(checkPath, &stateError)) {
            return fail(QStringLiteral("Cannot sync destination: %1").arg(stateError));
        }
    }
    notify(Phase::Syncing, QStringLiteral("Destination data is durable"), QString(), offset);

    if (abortRequested())
        return abortResult();

    {
        MountTable live = MountTable::load(&stateError);
        if (!live.isValid())
            return fail(QStringLiteral("Cannot inspect current mounts: %1").arg(stateError));
        if (!MovePlanner::revalidateMountState(plan, live, &stateError))
            return fail(QStringLiteral("Commit preflight failed: %1").arg(stateError));
    }
    if (!MovePlanner::revalidate(plan, mounts, &stateError))
        return fail(QStringLiteral("Commit preflight failed: %1").arg(stateError));

    const Component *manifestComponent = plan.findComponent(ComponentKind::Manifest);
    if (!manifestComponent) {
        return fail(QStringLiteral("Plan has no manifest component"));
    }
    if (pathExistsNoFollow(manifestComponent->destination)) {
        return fail(QStringLiteral("Destination manifest already exists: %1")
                        .arg(manifestComponent->destination));
    }
    const auto manifestData = fsutil::readFile(plan.game.manifestPath, 8 * 1024 * 1024, &stateError);
    if (!manifestData)
        return fail(QStringLiteral("Cannot read source manifest: %1").arg(stateError));
    setPhase(Phase::WritingManifest, QStringLiteral("Writing destination manifest"));
    if (!fsutil::ensureDir(QFileInfo(manifestComponent->destination).absolutePath(), &stateError))
        return fail(stateError);
    if (!fsutil::writeFileAtomic(manifestComponent->destination, *manifestData, 0644, &stateError))
        return fail(QStringLiteral("Cannot write destination manifest: %1").arg(stateError));
    notify(Phase::WritingManifest, QStringLiteral("Manifest written"), manifestComponent->destination,
         plan.totalBytes);

    setPhase(Phase::UpdatingVdf, QStringLiteral("Updating library database"));
    const QString vdfBackup = joinPath(JournalStore::backupDir(opId), QStringLiteral("libraryfolders.vdf"));
    LibraryFoldersFile libraryFile;
    if (!libraryFile.load(plan.libraryFoldersPath, &stateError)) {
        QFile::remove(manifestComponent->destination);
        return fail(QStringLiteral("Cannot reopen library database: %1").arg(stateError));
    }
    if (libraryFile.hash() != plan.snapshot.vdfHash) {
        QFile::remove(manifestComponent->destination);
        return fail(QStringLiteral("libraryfolders.vdf changed before commit; source was not modified"));
    }
    const Component *gameComponent = plan.findComponent(ComponentKind::Game);
    const quint64 installedBytes = gameComponent && gameComponent->bytes > 0
        ? gameComponent->bytes
        : plan.game.sizeOnDisk;
    if (!libraryFile.moveApp(plan.game.appId, plan.sourceLibraryPath, plan.destLibraryPath,
                             installedBytes, &stateError)) {
        QFile::remove(manifestComponent->destination);
        return fail(QStringLiteral("Cannot update library database: %1").arg(stateError));
    }
    if (!libraryFile.save(&stateError)) {
        QFile::remove(manifestComponent->destination);
        return fail(QStringLiteral("Cannot save library database: %1").arg(stateError));
    }
    notify(Phase::UpdatingVdf, QStringLiteral("Library database updated"), QString(), plan.totalBytes);

    setPhase(Phase::RemovingSourceManifest, QStringLiteral("Removing source manifest"));
    if (!QFile::remove(plan.game.manifestPath)) {
        // Best effort: restore the old VDF and remove the destination manifest so the
        // source installation remains authoritative.
        const auto backup = fsutil::readFile(vdfBackup, 32 * 1024 * 1024, nullptr);
        if (backup)
            fsutil::writeFileAtomic(plan.libraryFoldersPath, *backup, 0644, nullptr);
        QFile::remove(manifestComponent->destination);
        return fail(QStringLiteral("Cannot remove the source manifest. The library database was "
                                   "restored; source data is intact."));
    }
    notify(Phase::RemovingSourceManifest, QStringLiteral("Source manifest removed"), QString(), plan.totalBytes);

    setPhase(Phase::RemovingSourcePayload,
             plan.options.keepSourceBackup ? QStringLiteral("Moving source payload to backup")
                                           : QStringLiteral("Removing source payload"));
    QStringList cleanupErrors;
    QStringList backupPaths;
    bool cleanupAllowed = checkProcesses();
    if (!cleanupAllowed) {
        cleanupErrors.append(monitorError);
        logger.write(QStringLiteral("Cleanup skipped: %1").arg(monitorError));
    }
    for (const Component &component : plan.components) {
        if (!cleanupAllowed)
            break;
        if (component.kind == ComponentKind::Manifest || component.skip)
            continue;
        QString cleanupError;
        if (!removeSourcePayload(component.source, plan.game.appId, component.destination,
                                 plan.options.keepSourceBackup, &backupPaths, &cleanupError)) {
            cleanupErrors.append(QStringLiteral("%1: %2").arg(component.source, cleanupError));
            logger.write(QStringLiteral("Cleanup warning: %1").arg(cleanupError));
        }
        cleanupAllowed = checkProcesses();
    }

    journal.insert(QStringLiteral("backupPaths"), QJsonArray::fromStringList(backupPaths));
    journal.insert(QStringLiteral("state"), QStringLiteral("done"));
    journal.insert(QStringLiteral("phase"), phaseKey(Phase::Done));
    if (!cleanupErrors.isEmpty()) {
        journal.insert(QStringLiteral("state"), QStringLiteral("done-with-warnings"));
        journal.insert(QStringLiteral("error"),
                       QStringLiteral("Game was moved but source cleanup was incomplete"));
    }
    writeJournal();
    result.ok = true;
    result.cleanupErrors = cleanupErrors;
    result.backupPaths = backupPaths;
    result.error = cleanupErrors.isEmpty()
        ? QString()
        : QStringLiteral("Game moved, but some source files could not be cleaned up");
    notify(Phase::Done, result.error.isEmpty() ? QStringLiteral("Move complete") : result.error,
         QString(), plan.totalBytes);
    logger.write(QStringLiteral("Move complete%1").arg(result.error.isEmpty()
                                                          ? QString()
                                                          : QStringLiteral(" with warnings: %1").arg(result.error)));
    return result;
}

MovePlan MoveExecutor::planFromJournal(const QJsonObject &journal, QString *error)
{
    const auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return MovePlan{};
    };
    const QString opId = journal.value(QStringLiteral("opId")).toString();
    if (!JournalStore::isValidOperationId(opId))
        return fail(QStringLiteral("Invalid operation id in journal"));
    const QString sourceLibrary = journal.value(QStringLiteral("sourceLibrary")).toString();
    const QString destLibrary = journal.value(QStringLiteral("destinationLibrary")).toString();
    const QString vdf = journal.value(QStringLiteral("vdf")).toString();
    const QString manifest = journal.value(QStringLiteral("manifest")).toString();
    if (sourceLibrary.isEmpty() || !QDir::isAbsolutePath(sourceLibrary)
        || destLibrary.isEmpty() || !QDir::isAbsolutePath(destLibrary)
        || vdf.isEmpty() || !QDir::isAbsolutePath(vdf)
        || manifest.isEmpty() || !QDir::isAbsolutePath(manifest)) {
        return fail(QStringLiteral("Journal is missing absolute library or manifest paths"));
    }
    if (QFileInfo(manifest).absolutePath()
        != QDir(sourceLibrary + QStringLiteral("/steamapps")).absolutePath()) {
        return fail(QStringLiteral("Journal manifest is outside the source library"));
    }

    MovePlan plan;
    plan.game.appId = static_cast<quint32>(journal.value(QStringLiteral("appId")).toInteger());
    plan.game.name = journal.value(QStringLiteral("name")).toString();
    plan.game.installDir = journal.value(QStringLiteral("installDir")).toString();
    plan.game.manifestPath = manifest;
    plan.game.libraryPath = sourceLibrary;
    plan.sourceLibraryPath = sourceLibrary;
    plan.destLibraryPath = destLibrary;
    plan.libraryFoldersPath = vdf;
    plan.options.fullVerify = journal.value(QStringLiteral("fullVerify")).toBool(true);
    plan.options.keepSourceBackup = journal.value(QStringLiteral("keepSourceBackup")).toBool(false);

    const QJsonArray components = journal.value(QStringLiteral("components")).toArray();
    if (components.isEmpty())
        return fail(QStringLiteral("Journal has no components to resume"));
    quint64 total = 0;
    quint64 gameBytes = 0;
    bool hasManifest = false;
    for (const QJsonValue &value : components) {
        const QJsonObject object = value.toObject();
        Component component;
        const int kind = object.value(QStringLiteral("kind")).toInt();
        if (kind < static_cast<int>(ComponentKind::Game)
            || kind > static_cast<int>(ComponentKind::WorkshopManifest)) {
            return fail(QStringLiteral("Journal component has an invalid kind"));
        }
        component.kind = static_cast<ComponentKind>(kind);
        component.source = object.value(QStringLiteral("source")).toString();
        component.destination = object.value(QStringLiteral("destination")).toString();
        component.bytes = static_cast<quint64>(object.value(QStringLiteral("bytes")).toInteger());
        component.required = object.value(QStringLiteral("required")).toBool(true);
        component.skip = object.value(QStringLiteral("skip")).toBool(false);
        component.note = object.value(QStringLiteral("note")).toString();
        if (!component.skip
            && (component.source.isEmpty() || !QDir::isAbsolutePath(component.source)
                || component.destination.isEmpty()
                || !QDir::isAbsolutePath(component.destination))) {
            return fail(QStringLiteral("Journal component has an invalid source or destination path"));
        }
        if (component.kind == ComponentKind::Manifest)
            hasManifest = true;
        if (!component.skip && component.kind != ComponentKind::Manifest) {
            if (std::numeric_limits<quint64>::max() - total < component.bytes)
                return fail(QStringLiteral("Journal component sizes overflow"));
            total += component.bytes;
        }
        if (component.kind == ComponentKind::Game)
            gameBytes = component.bytes;
        plan.components.append(component);
    }
    if (!hasManifest)
        return fail(QStringLiteral("Journal has no manifest component"));
    plan.totalBytes = total;
    plan.game.sizeOnDisk = gameBytes;
    return plan;
}

bool MoveExecutor::canResume(const QJsonObject &journal, QString *reason)
{
    const auto no = [&](const QString &message) {
        if (reason)
            *reason = message;
        return false;
    };
    if (journal.isEmpty())
        return no(QStringLiteral("Operation journal is empty"));
    const QString opId = journal.value(QStringLiteral("opId")).toString();
    if (!JournalStore::isValidOperationId(opId))
        return no(QStringLiteral("Invalid operation id"));
    if (journal.value(QStringLiteral("state")).toString() == QStringLiteral("done")
        || journal.value(QStringLiteral("state")).toString() == QStringLiteral("done-with-warnings")
        || journal.value(QStringLiteral("state")).toString() == QStringLiteral("rolled-back")) {
        return no(QStringLiteral("Operation is already finished"));
    }
    if (!phaseAllowsResume(journal.value(QStringLiteral("phase")).toString())) {
        return no(QStringLiteral("The manifest commit has started; roll back instead of resuming"));
    }

    QString parseError;
    const MovePlan plan = planFromJournal(journal, &parseError);
    if (!parseError.isEmpty())
        return no(parseError);

    const QString backupDir = journal.value(QStringLiteral("backupDir")).toString();
    const QString operationsRoot = canonicalOrAbsolute(JournalStore::operationsDir());
    if (backupDir.isEmpty()
        || !pathIsWithin(canonicalOrAbsolute(backupDir), operationsRoot)
        || canonicalOrAbsolute(backupDir)
            != canonicalOrAbsolute(JournalStore::backupDir(opId))) {
        return no(QStringLiteral("Journal backup directory is missing or unsafe"));
    }
    const QString vdfBackup = joinPath(backupDir, QStringLiteral("libraryfolders.vdf"));
    const QString manifestBackup = joinPath(backupDir, manifestBackupName(plan.game.appId));
    if (!QFileInfo::exists(vdfBackup) || !QFileInfo::exists(manifestBackup))
        return no(QStringLiteral("The original VDF or manifest backup is missing"));
    if (!QFileInfo::exists(plan.game.manifestPath))
        return no(QStringLiteral("The source manifest no longer exists"));
    if (!QFileInfo::exists(plan.sourceLibraryPath) || !QFileInfo::exists(plan.destLibraryPath))
        return no(QStringLiteral("The source or destination library is no longer mounted"));
    if (!QFileInfo::exists(plan.libraryFoldersPath))
        return no(QStringLiteral("The Steam library database no longer exists"));

    const QString steamRoot = QFileInfo(QFileInfo(plan.libraryFoldersPath).absolutePath()).absolutePath();
    const QString allowedLibrary = canonicalOrAbsolute(plan.destLibraryPath);
    const QString allowedRoot = canonicalOrAbsolute(steamRoot);
    for (const Component &component : plan.components) {
        if (component.skip)
            continue;
        const QString destination = canonicalOrAbsolute(component.destination);
        const bool inDestination = pathIsWithin(destination, allowedLibrary);
        // A shared Proton prefix may intentionally stay at its source path under
        // the Steam root. Every other component must live in the destination
        // library; "somewhere under the Steam root" is not specific enough.
        const bool sharedPrefix = component.kind == ComponentKind::Prefix
            && destination == canonicalOrAbsolute(component.source);
        if (!inDestination
            && !(sharedPrefix && pathIsWithin(destination, allowedRoot))) {
            return no(QStringLiteral("A journal destination escapes the destination library"));
        }
        if (component.kind == ComponentKind::Manifest && QFileInfo::exists(component.destination))
            return no(QStringLiteral("The destination manifest already exists; roll back instead"));
    }

    const auto backupVdf = fsutil::readFile(vdfBackup, 32 * 1024 * 1024);
    const auto backupManifest = fsutil::readFile(manifestBackup, 8 * 1024 * 1024);
    if (!backupVdf || !backupManifest)
        return no(QStringLiteral("The original VDF or manifest backup cannot be read"));
    const auto currentVdf = fsutil::readFile(plan.libraryFoldersPath, 32 * 1024 * 1024);
    const auto currentManifest = fsutil::readFile(plan.game.manifestPath, 8 * 1024 * 1024);
    if (!currentVdf || !currentManifest)
        return no(QStringLiteral("The current library database or manifest cannot be read"));
    if (*backupVdf != *currentVdf)
        return no(QStringLiteral("libraryfolders.vdf changed since the operation started"));
    if (*backupManifest != *currentManifest)
        return no(QStringLiteral("The source manifest changed since the operation started"));
    return true;
}

MoveResult MoveExecutor::resumeOperation(const QJsonObject &journal, const MountTable &mounts,
                                         const ProgressFn &progress, const CancelFn &cancelled,
                                         const PauseFn &paused)
{
    MoveResult result;
    QString error;
    if (!canResume(journal, &error)) {
        result.error = error;
        return result;
    }
    MovePlan plan = planFromJournal(journal, &error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QString backupDir = journal.value(QStringLiteral("backupDir")).toString();
    const QString vdfBackup = joinPath(backupDir, QStringLiteral("libraryfolders.vdf"));
    const QString manifestBackup = joinPath(backupDir, manifestBackupName(plan.game.appId));
    const auto vdfData = fsutil::readFile(vdfBackup, 32 * 1024 * 1024, &error);
    if (!vdfData) {
        result.error = error;
        return result;
    }
    const auto manifestData = fsutil::readFile(manifestBackup, 8 * 1024 * 1024, &error);
    if (!manifestData) {
        result.error = error;
        return result;
    }
    plan.snapshot.vdfHash = QCryptographicHash::hash(*vdfData, QCryptographicHash::Sha256);
    plan.snapshot.manifestHash = QCryptographicHash::hash(*manifestData, QCryptographicHash::Sha256);
    QFileInfo manifestInfo(plan.game.manifestPath);
    plan.snapshot.manifestSize = manifestInfo.size();
    plan.snapshot.manifestMtime = manifestInfo.lastModified().toSecsSinceEpoch();
    struct stat st {};
    if (::stat(QFile::encodeName(plan.sourceLibraryPath).constData(), &st) == 0)
        plan.snapshot.sourceDevice = static_cast<quint64>(st.st_dev);
    if (::stat(QFile::encodeName(plan.destLibraryPath).constData(), &st) == 0)
        plan.snapshot.destinationDevice = static_cast<quint64>(st.st_dev);
    return execute(plan, mounts, progress, cancelled, paused, journal);
}

QList<QJsonObject> MoveExecutor::unfinishedOperations()
{
    QList<QJsonObject> result;
    for (const QJsonObject &journal : JournalStore::listOperations()) {
        const QString state = journal.value(QStringLiteral("state")).toString();
        if (state == QStringLiteral("done")
            || state == QStringLiteral("done-with-warnings")
            || state == QStringLiteral("rolled-back"))
            continue;
        result.append(journal);
    }
    return result;
}

bool MoveExecutor::rollbackOperation(const QJsonObject &journal, QString *error)
{
    if (::geteuid() == 0) {
        if (error)
            *error = QStringLiteral("Refusing to roll back as root");
        return false;
    }

    const QString opId = journal.value(QStringLiteral("opId")).toString();
    if (!JournalStore::isValidOperationId(opId)) {
        if (error)
            *error = QStringLiteral("Refusing rollback: invalid operation id in journal");
        return false;
    }

    const QString vdf = journal.value(QStringLiteral("vdf")).toString();
    const QString destinationLibrary = journal.value(QStringLiteral("destinationLibrary")).toString();
    if (destinationLibrary.isEmpty() || !QDir::isAbsolutePath(destinationLibrary)
        || vdf.isEmpty() || !QDir::isAbsolutePath(vdf)
        || QFileInfo(vdf).fileName() != QStringLiteral("libraryfolders.vdf")) {
        if (error)
            *error = QStringLiteral("Refusing rollback: malformed journal paths");
        return false;
    }
    const QString steamRoot = QFileInfo(QFileInfo(vdf).absolutePath()).absolutePath();
    const QString allowedLibrary = canonicalOrAbsolute(destinationLibrary);
    const QString allowedRoot = canonicalOrAbsolute(steamRoot);
    if (allowedLibrary.isEmpty() || allowedRoot.isEmpty()) {
        if (error)
            *error = QStringLiteral("Refusing rollback: cannot resolve journal paths");
        return false;
    }

    const auto allowedPath = [&](const QString &path) {
        if (path.isEmpty())
            return false;
        const QString canonical = canonicalOrAbsolute(path);
        return pathIsWithin(canonical, allowedLibrary) || pathIsWithin(canonical, allowedRoot);
    };

    QStringList prefixes;
    QStringList gameDirs;
    const QJsonArray components = journal.value(QStringLiteral("components")).toArray();
    for (const QJsonValue &value : components) {
        const QJsonObject component = value.toObject();
        const int kind = component.value(QStringLiteral("kind")).toInt();
        const QString source = component.value(QStringLiteral("source")).toString();
        const QString destination = component.value(QStringLiteral("destination")).toString();
        if (!destination.isEmpty() && !allowedPath(destination)) {
            if (error)
                *error = QStringLiteral("Refusing rollback: journal path escapes the destination/Steam root");
            return false;
        }
        if (kind == static_cast<int>(ComponentKind::Prefix)) {
            if (!source.isEmpty())
                prefixes.append(source);
            if (!destination.isEmpty())
                prefixes.append(destination);
        } else if (kind == static_cast<int>(ComponentKind::Game) && !destination.isEmpty()) {
            gameDirs.append(destination);
        }
    }

    const ProcessUsage usage = inspectProcesses(prefixes, gameDirs);
    if (usage.steamRunning) {
        if (error)
            *error = QStringLiteral("Close Steam before rolling back an operation");
        return false;
    }
    if (usage.prefixInUse || usage.gameInUse) {
        if (error)
            *error = QStringLiteral("Close the running game/Proton processes before rolling back");
        return false;
    }

    const QString phase = journal.value(QStringLiteral("phase")).toString();
    if (phase == phaseName(Phase::RemovingSourceManifest)
        || phase == phaseName(Phase::RemovingSourcePayload) || phase == phaseName(Phase::Done)) {
        if (error)
            *error = QStringLiteral("The source manifest was already removed; automatic rollback is not safe.");
        return false;
    }

    const QString sourceManifest = journal.value(QStringLiteral("manifest")).toString();
    if (!QFileInfo::exists(sourceManifest)) {
        if (error)
            *error = QStringLiteral("The source manifest no longer exists; cannot roll back safely.");
        return false;
    }

    const QString backup = journal.value(QStringLiteral("backupDir")).toString();
    const QString operationsRoot = canonicalOrAbsolute(JournalStore::operationsDir());
    if (backup.isEmpty()
        || !pathIsWithin(canonicalOrAbsolute(backup), operationsRoot)) {
        if (error)
            *error = QStringLiteral("Refusing rollback: journal backup path escapes the state directory");
        return false;
    }
    const QString vdfBackup = joinPath(backup, QStringLiteral("libraryfolders.vdf"));

    // Restore the library database first. If this fails, leave all destination
    // data untouched: Steam must still recognize the source library.
    if (!QFileInfo::exists(vdfBackup)) {
        if (error)
            *error = QStringLiteral("Refusing rollback: the libraryfolders.vdf backup is missing");
        return false;
    }
    const auto data = fsutil::readFile(vdfBackup, 32 * 1024 * 1024, error);
    if (!data)
        return false;
    if (!fsutil::writeFileAtomic(vdf, *data, 0644, error))
        return false;

    QStringList failures;
    for (const QJsonValue &value : components) {
        const QJsonObject component = value.toObject();
        if (component.value(QStringLiteral("skip")).toBool())
            continue;
        const int kind = component.value(QStringLiteral("kind")).toInt();
        if (kind == static_cast<int>(ComponentKind::Manifest))
            continue;
        const QString destination = component.value(QStringLiteral("destination")).toString();
        if (destination.isEmpty())
            continue;
        QString removeError;
        if (!fsutil::removeTree(destination, &removeError))
            failures.append(removeError);
    }

    const QJsonObject manifestComponent = [&]() {
        for (const QJsonValue &value : components) {
            const QJsonObject object = value.toObject();
            if (object.value(QStringLiteral("kind")).toInt() == static_cast<int>(ComponentKind::Manifest))
                return object;
        }
        return QJsonObject();
    }();
    const QString destinationManifest =
        manifestComponent.value(QStringLiteral("destination")).toString();
    if (!destinationManifest.isEmpty() && QFileInfo::exists(destinationManifest)
        && !QFile::remove(destinationManifest)) {
        if (error)
            *error = QStringLiteral("Cannot remove the destination manifest");
        return false;
    }

    if (!failures.isEmpty()) {
        if (error)
            *error = QStringLiteral("Rollback incomplete: %1").arg(failures.join(QStringLiteral("; ")));
        return false;
    }

    QJsonObject updated = journal;
    updated.insert(QStringLiteral("state"), QStringLiteral("rolled-back"));
    updated.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    QString writeError;
    if (!JournalStore::writeObject(JournalStore::journalPath(opId), updated, &writeError)) {
        if (error)
            *error = writeError;
        return false;
    }
    if (error)
        error->clear();
    return true;
}
} // namespace slm
