// SPDX-License-Identifier: GPL-3.0-or-later
#include "copyengine.h"

#include "filetree.h"

#include "fileops.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace slm {

namespace {

constexpr qint64 kBufferSize = 4 * 1024 * 1024;
constexpr qint64 kProgressIntervalMs = 50;

QString errnoString()
{
    return QString::fromLocal8Bit(::strerror(errno));
}

bool waitWhilePaused(const CopyEngine::CancelFn &cancelled, const CopyEngine::PauseFn &paused)
{
    while (paused && paused()) {
        if (cancelled && cancelled())
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

bool lstatPath(const QString &path, struct stat *st)
{
    return ::lstat(QFile::encodeName(path).constData(), st) == 0;
}

bool isDirectory(const struct stat &st)
{
    return S_ISDIR(st.st_mode) != 0;
}

bool ensureDestinationDirectory(const QString &path, QString *error)
{
    struct stat st {};
    if (lstatPath(path, &st)) {
        if (isDirectory(st))
            return true;
        if (error)
            *error = QStringLiteral("Destination path exists but is not a directory: %1").arg(path);
        return false;
    }
    if (::mkdir(QFile::encodeName(path).constData(), 0755) != 0) {
        if (error)
            *error = QStringLiteral("Cannot create directory %1: %2").arg(path, errnoString());
        return false;
    }
    return true;
}

bool unlinkIfExists(const QString &path, QString *error)
{
    struct stat st {};
    if (!lstatPath(path, &st))
        return true;
    if (isDirectory(st)) {
        if (error)
            *error = QStringLiteral("Refusing to replace a directory with a file: %1").arg(path);
        return false;
    }
    if (::unlink(QFile::encodeName(path).constData()) != 0) {
        if (error)
            *error = QStringLiteral("Cannot replace %1: %2").arg(path, errnoString());
        return false;
    }
    return true;
}

bool applyTimesAndMode(const QString &path, const FileEntry &entry, bool symlink)
{
    const QByteArray encoded = QFile::encodeName(path);
    struct timespec times[2];
    times[0].tv_sec = entry.mtimeSec;
    times[0].tv_nsec = entry.mtimeNsec;
    times[1].tv_sec = entry.mtimeSec;
    times[1].tv_nsec = entry.mtimeNsec;
    if (!symlink) {
        if (::chmod(encoded.constData(), static_cast<mode_t>(entry.mode)) != 0) {
            // NTFS/FUSE may not support Unix modes. Content correctness is more important.
        }
    }
    if (::utimensat(AT_FDCWD, encoded.constData(), times,
                    symlink ? AT_SYMLINK_NOFOLLOW : 0)
        != 0) {
        // Filesystem timestamp granularity is not fatal.
    }
    return true;
}

bool copyRegularFile(const QString &source, const QString &destination, const FileEntry &entry,
                     const CopyEngine::ProgressFn &progress, qint64 *bytesCopied,
                     const CopyEngine::CancelFn &cancelled, const CopyEngine::PauseFn &paused,
                     std::vector<char> &buffer, QString *error)
{
    const QByteArray sourceName = QFile::encodeName(source);
    const int input = ::open(sourceName.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (input < 0) {
        if (error)
            *error = QStringLiteral("Cannot open %1: %2").arg(source, errnoString());
        return false;
    }

    // Game payloads are read and written front-to-back once. Tell the kernel so
    // it can drop readahead and writeback the destination in order.
    (void)::posix_fadvise(input, 0, 0, POSIX_FADV_SEQUENTIAL);
    const QString temporary = QFileInfo(destination).absolutePath() + QStringLiteral("/.slm-part-")
        + QString::number(::getpid()) + QLatin1Char('-')
        + QString::number(static_cast<qulonglong>(QDateTime::currentMSecsSinceEpoch()));
    const QByteArray temporaryName = QFile::encodeName(temporary);
    const int output = ::open(temporaryName.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output < 0) {
        ::close(input);
        if (error)
            *error = QStringLiteral("Cannot create temporary file %1: %2").arg(temporary, errnoString());
        return false;
    }

    (void)::posix_fadvise(output, 0, 0, POSIX_FADV_SEQUENTIAL);

    bool ok = true;
    bool cloned = false;
    // On copy-on-write filesystems (Btrfs, XFS with reflinks, ...), cloning the
    // extents is dramatically faster than reading and rewriting every byte and
    // still produces a fully independent destination file. Cross-filesystem
    // moves simply fail the ioctl and fall back to the normal copy loop.
    if ((!cancelled || !cancelled()) && waitWhilePaused(cancelled, paused)) {
        if (::ioctl(output, FICLONE, input) == 0) {
            cloned = true;
            *bytesCopied += static_cast<qint64>(entry.size);
            struct stat clonedStat {};
            if (::fstat(output, &clonedStat) != 0
                || static_cast<quint64>(clonedStat.st_size) != entry.size) {
                if (error)
                    *error = QStringLiteral("Reflink clone produced an unexpected size for %1")
                                 .arg(source);
                ok = false;
            } else if (progress) {
                progress(static_cast<quint64>(*bytesCopied), entry.relativePath);
            }
        } else {
            // Not a reflink-capable destination. Make sure a failed ioctl did
            // not leave a partially-created extent before the normal copy loop.
            (void)::ftruncate(output, 0);
            (void)::lseek(output, 0, SEEK_SET);
        }
    } else {
        ok = false;
        if (error && error->isEmpty())
            *error = QStringLiteral("Cancelled by user");
    }

    QElapsedTimer timer;
    timer.start();
    while (ok && !cloned) {
        if (cancelled && cancelled()) {
            if (error)
                *error = QStringLiteral("Cancelled by user");
            ok = false;
            break;
        }
        if (!waitWhilePaused(cancelled, paused)) {
            if (error)
                *error = QStringLiteral("Cancelled by user");
            ok = false;
            break;
        }
        const ssize_t read = ::read(input, buffer.data(), buffer.size());
        if (read < 0) {
            if (errno == EINTR)
                continue;
            if (error)
                *error = QStringLiteral("Cannot read %1: %2").arg(source, errnoString());
            ok = false;
            break;
        }
        if (read == 0)
            break;

        ssize_t written = 0;
        while (written < read) {
            const ssize_t result =
                ::write(output, buffer.data() + written, static_cast<size_t>(read - written));
            if (result < 0) {
                if (errno == EINTR)
                    continue;
                if (error)
                    *error = QStringLiteral("Cannot write %1: %2").arg(temporary, errnoString());
                ok = false;
                break;
            }
            written += result;
        }
        if (!ok)
            break;

        *bytesCopied += read;
        if (timer.elapsed() >= kProgressIntervalMs) {
            if (progress)
                progress(static_cast<quint64>(*bytesCopied), entry.relativePath);
            timer.restart();
        }
    }

    // Per-file fsync would make large game moves extremely slow. The executor
    // calls syncfs() on every destination filesystem before committing.
    if (ok) {
        if (::fchmod(output, static_cast<mode_t>(entry.mode)) != 0) {
            // Best-effort on NTFS/FUSE.
        }
        struct timespec times[2];
        times[0].tv_sec = entry.mtimeSec;
        times[0].tv_nsec = entry.mtimeNsec;
        times[1].tv_sec = entry.mtimeSec;
        times[1].tv_nsec = entry.mtimeNsec;
        if (::futimens(output, times) != 0) {
            // Best-effort.
        }
    }

    if (::close(output) != 0 && ok) {
        if (error)
            *error = QStringLiteral("Cannot close %1: %2").arg(temporary, errnoString());
        ok = false;
    }
    ::close(input);

    if (ok) {
        if (!unlinkIfExists(destination, error)) {
            ::unlink(temporaryName.constData());
            return false;
        }
        if (::rename(temporaryName.constData(), QFile::encodeName(destination).constData()) != 0) {
            if (error)
                *error = QStringLiteral("Cannot finalize %1: %2").arg(destination, errnoString());
            ::unlink(temporaryName.constData());
            ok = false;
        }
    } else {
        ::unlink(temporaryName.constData());
    }

    if (ok && progress)
        progress(static_cast<quint64>(*bytesCopied), entry.relativePath);
    return ok;
}

bool destinationMatches(const FileEntry &entry, const QString &destination)
{
    struct stat st {};
    if (!lstatPath(destination, &st))
        return false;

    switch (entry.type) {
    case FileEntryType::Directory:
        return isDirectory(st);
    case FileEntryType::Symlink: {
        if (!S_ISLNK(st.st_mode))
            return false;
        const auto target = fsutil::readSymlinkTarget(destination);
        return target && *target == entry.symlinkTarget.toUtf8();
    }
    case FileEntryType::File:
        return S_ISREG(st.st_mode) && static_cast<quint64>(st.st_size) == entry.size
            && st.st_mtim.tv_sec == entry.mtimeSec
            && (entry.mtimeNsec == 0 || st.st_mtim.tv_nsec == entry.mtimeNsec);
    case FileEntryType::Other:
        return false;
    }
    return false;
}

bool hashFile(const QString &path, QByteArray *result, QString *error,
              const CopyEngine::CancelFn &cancelled)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (cancelled && cancelled()) {
            if (error)
                *error = QStringLiteral("Cancelled by user");
            return false;
        }
        const QByteArray chunk = file.read(kBufferSize);
        if (chunk.isEmpty() && file.error() != QFile::NoError) {
            if (error)
                *error = QStringLiteral("Cannot hash %1: %2").arg(path, file.errorString());
            return false;
        }
        hash.addData(chunk);
    }
    if (cancelled && cancelled()) {
        if (error)
            *error = QStringLiteral("Cancelled by user");
        return false;
    }
    *result = hash.result();
    return true;
}

} // namespace

bool CopyEngine::copyTree(const QString &source, const QString &destination,
                          const ProgressFn &progress, const CancelFn &cancelled,
                          qint64 *bytesCopiedOut, QString *error, const PauseFn &paused)
{
    FileTree tree = FileTree::scan(source);
    std::vector<char> copyBuffer(static_cast<size_t>(kBufferSize));
    if (!tree.ok) {
        if (error)
            *error = tree.error;
        return false;
    }
    if (!ensureDestinationDirectory(destination, error))
        return false;

    struct stat sourceRoot {};
    if (!lstatPath(source, &sourceRoot)) {
        if (error)
            *error = QStringLiteral("Cannot inspect source directory %1").arg(source);
        return false;
    }

    QVector<QPair<QString, FileEntry>> directories;
    FileEntry rootEntry;
    rootEntry.relativePath = QStringLiteral(".");
    rootEntry.type = FileEntryType::Directory;
    rootEntry.mode = static_cast<quint32>(sourceRoot.st_mode & 07777);
    rootEntry.mtimeSec = sourceRoot.st_mtim.tv_sec;
    rootEntry.mtimeNsec = sourceRoot.st_mtim.tv_nsec;
    directories.append({destination, rootEntry});

    QHash<QString, QString> hardLinks;
    qint64 bytesCopied = 0;
    bool ok = true;

    for (const FileEntry &entry : tree.entries) {
        if (cancelled && cancelled()) {
            if (error)
                *error = QStringLiteral("Cancelled by user");
            return false;
        }
        if (!waitWhilePaused(cancelled, paused)) {
            if (error)
                *error = QStringLiteral("Cancelled by user");
            return false;
        }

        const QString destPath = destination + QLatin1Char('/') + entry.relativePath;
        const QFileInfo destInfo(destPath);

        switch (entry.type) {
        case FileEntryType::Directory: {
            struct stat st {};
            if (lstatPath(destPath, &st)) {
                if (!isDirectory(st)) {
                    if (error)
                        *error = QStringLiteral("Destination is not a directory: %1").arg(destPath);
                    ok = false;
                    break;
                }
            } else if (::mkdir(QFile::encodeName(destPath).constData(), 0755) != 0
                       && errno != EEXIST) {
                if (error)
                    *error = QStringLiteral("Cannot create %1: %2").arg(destPath, errnoString());
                ok = false;
                break;
            }
            directories.append({destPath, entry});
            break;
        }
        case FileEntryType::Symlink: {
            struct stat st {};
            bool exists = lstatPath(destPath, &st);
            if (exists && S_ISLNK(st.st_mode)) {
                const auto existingTarget = fsutil::readSymlinkTarget(destPath);
                if (existingTarget && *existingTarget == entry.symlinkTarget.toUtf8())
                    break;
            }
            if (exists && !S_ISLNK(st.st_mode)) {
                if (error)
                    *error = QStringLiteral("Cannot replace non-symlink at %1").arg(destPath);
                ok = false;
                break;
            }
            if (exists && ::unlink(QFile::encodeName(destPath).constData()) != 0) {
                if (error)
                    *error = QStringLiteral("Cannot remove old symlink %1: %2").arg(destPath, errnoString());
                ok = false;
                break;
            }
            if (::symlink(entry.symlinkTarget.toUtf8().constData(),
                          QFile::encodeName(destPath).constData())
                != 0) {
                if (error)
                    *error = QStringLiteral("Cannot create symlink %1: %2").arg(destPath, errnoString());
                ok = false;
                break;
            }
            applyTimesAndMode(destPath, entry, true);
            break;
        }
        case FileEntryType::File: {
            struct stat st {};
            if (lstatPath(destPath, &st) && destinationMatches(entry, destPath)) {
                bytesCopied += static_cast<qint64>(entry.size);
                if (progress)
                    progress(static_cast<quint64>(bytesCopied), entry.relativePath);
                break;
            }
            if (lstatPath(destPath, &st) && S_ISDIR(st.st_mode)) {
                if (error)
                    *error = QStringLiteral("Refusing to replace directory %1").arg(destPath);
                ok = false;
                break;
            }

            const QString inodeKey = QStringLiteral("%1:%2").arg(entry.device).arg(entry.inode);
            if (entry.linkCount > 1 && hardLinks.contains(inodeKey)) {
                const QString existing = hardLinks.value(inodeKey);
                if (!unlinkIfExists(destPath, error)) {
                    ok = false;
                    break;
                }
                if (::link(QFile::encodeName(existing).constData(),
                           QFile::encodeName(destPath).constData())
                    != 0) {
                    if (error)
                        *error = QStringLiteral("Cannot create hard link %1: %2")
                                     .arg(destPath, errnoString());
                    ok = false;
                    break;
                }
                break;
            }

            if (!copyRegularFile(source + QLatin1Char('/') + entry.relativePath, destPath, entry,
                                 progress, &bytesCopied, cancelled, paused, copyBuffer, error)) {
                ok = false;
                break;
            }
            if (entry.linkCount > 1)
                hardLinks.insert(inodeKey, destPath);
            break;
        }
        case FileEntryType::Other:
            if (error)
                *error = QStringLiteral("Unsupported file type in source: %1").arg(entry.relativePath);
            ok = false;
            break;
        }

        if (!ok)
            break;
    }

    if (ok) {
        for (int i = directories.size() - 1; i >= 0; --i) {
            const auto &[path, entry] = directories.at(i);
            applyTimesAndMode(path, entry, false);
        }
    }

    if (bytesCopiedOut)
        *bytesCopiedOut = bytesCopied;
    if (ok && progress)
        progress(static_cast<quint64>(bytesCopied), QString());
    return ok;
}

bool CopyEngine::verifyTree(const QString &source, const QString &destination, bool fullHash,
                            QString *error, const CancelFn &cancelled)
{
    if (cancelled && cancelled()) {
        if (error)
            *error = QStringLiteral("Cancelled by user");
        return false;
    }
    FileTree sourceTree = FileTree::scan(source);
    if (cancelled && cancelled()) {
        if (error)
            *error = QStringLiteral("Cancelled by user");
        return false;
    }
    if (!sourceTree.ok) {
        if (error)
            *error = sourceTree.error;
        return false;
    }
    FileTree destTree = FileTree::scan(destination);
    if (cancelled && cancelled()) {
        if (error)
            *error = QStringLiteral("Cancelled by user");
        return false;
    }
    if (!destTree.ok) {
        if (error)
            *error = destTree.error;
        return false;
    }

    QHash<QString, const FileEntry *> destEntries;
    int extra = 0;
    for (const FileEntry &entry : destTree.entries) {
        if (QFileInfo(entry.relativePath).fileName().startsWith(QStringLiteral(".slm-part-")))
            continue;
        destEntries.insert(entry.relativePath, &entry);
        extra++;
    }

    for (const FileEntry &sourceEntry : sourceTree.entries) {
        if (cancelled && cancelled()) {
            if (error)
                *error = QStringLiteral("Cancelled by user");
            return false;
        }
        const auto found = destEntries.constFind(sourceEntry.relativePath);
        if (found == destEntries.constEnd()) {
            if (error)
                *error = QStringLiteral("Missing at destination: %1").arg(sourceEntry.relativePath);
            return false;
        }
        const FileEntry &destEntry = *found.value();
        if (sourceEntry.type != destEntry.type) {
            if (error)
                *error = QStringLiteral("Type mismatch: %1").arg(sourceEntry.relativePath);
            return false;
        }
        switch (sourceEntry.type) {
        case FileEntryType::File:
            if (sourceEntry.size != destEntry.size) {
                if (error)
                    *error = QStringLiteral("Size mismatch: %1").arg(sourceEntry.relativePath);
                return false;
            }
            if (fullHash) {
                QByteArray sourceHash;
                QByteArray destHash;
                const QString sp = source + QLatin1Char('/') + sourceEntry.relativePath;
                const QString dp = destination + QLatin1Char('/') + destEntry.relativePath;
                if (!hashFile(sp, &sourceHash, error, cancelled)
                    || !hashFile(dp, &destHash, error, cancelled))
                    return false;
                if (sourceHash != destHash) {
                    if (error)
                        *error = QStringLiteral("Checksum mismatch: %1").arg(sourceEntry.relativePath);
                    return false;
                }
            }
            break;
        case FileEntryType::Symlink:
            if (sourceEntry.symlinkTarget != destEntry.symlinkTarget) {
                if (error)
                    *error = QStringLiteral("Symlink mismatch: %1").arg(sourceEntry.relativePath);
                return false;
            }
            break;
        case FileEntryType::Directory:
            break;
        case FileEntryType::Other:
            if (error)
                *error = QStringLiteral("Unsupported file type: %1").arg(sourceEntry.relativePath);
            return false;
        }
        destEntries.remove(sourceEntry.relativePath);
        extra--;
    }

    if (!destEntries.isEmpty() || extra != 0) {
        const QString path = destEntries.isEmpty() ? QString() : destEntries.constBegin().key();
        if (error) {
            *error = QStringLiteral("Destination has unexpected extra files%1")
                         .arg(path.isEmpty() ? QString() : QStringLiteral(" (for example: %1)").arg(path));
        }
        return false;
    }
    return true;
}

} // namespace slm
