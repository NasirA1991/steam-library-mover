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
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <mutex>
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

void addZeroBytes(QCryptographicHash *hash, quint64 count)
{
    static const QByteArray zeros(64 * 1024, '\0');
    while (count > 0) {
        const int chunk = static_cast<int>(std::min<quint64>(count, zeros.size()));
        hash->addData(QByteArrayView(zeros.constData(), chunk));
        count -= static_cast<quint64>(chunk);
    }
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

bool copySparseExtents(int input, int output, quint64 size, const FileEntry &entry,
                        const CopyEngine::ProgressFn &progress, qint64 baseBytes,
                        const CopyEngine::CancelFn &cancelled, const CopyEngine::PauseFn &paused,
                        std::vector<char> &buffer, QString *error, bool *supported,
                        QCryptographicHash *hash)
{
    *supported = false;
    off_t position = 0;
    while (position < static_cast<off_t>(size)) {
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

        errno = 0;
        const off_t data = ::lseek(input, position, SEEK_DATA);
        if (data < 0) {
            if (errno == ENXIO) {
                if (hash)
                    addZeroBytes(hash, size - static_cast<quint64>(position));
                *supported = true;
                return true;
            }
            if (errno == EINVAL) {
                // The source filesystem does not expose holes. Fall back to a
                // normal sequential copy.
                return false;
            }
            if (error)
                *error = QStringLiteral("Cannot inspect sparse extents in %1: %2")
                             .arg(entry.relativePath, errnoString());
            return false;
        }
        if (data >= static_cast<off_t>(size)) {
            if (hash)
                addZeroBytes(hash, size - static_cast<quint64>(position));
            *supported = true;
            return true;
        }
        if (hash && data > position)
            addZeroBytes(hash, static_cast<quint64>(data - position));

        errno = 0;
        off_t hole = ::lseek(input, data, SEEK_HOLE);
        if (hole < 0) {
            if (errno == EINVAL)
                return false;
            if (error)
                *error = QStringLiteral("Cannot inspect sparse extents in %1: %2")
                             .arg(entry.relativePath, errnoString());
            return false;
        }
        if (hole > static_cast<off_t>(size))
            hole = static_cast<off_t>(size);

        off_t cursor = data;
        while (cursor < hole) {
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
            const size_t wanted = std::min(buffer.size(), static_cast<size_t>(hole - cursor));
            const ssize_t read = ::pread(input, buffer.data(), wanted, cursor);
            if (read < 0) {
                if (errno == EINTR)
                    continue;
                if (error)
                    *error = QStringLiteral("Cannot read %1: %2").arg(entry.relativePath, errnoString());
                return false;
            }
            if (read == 0) {
                if (error)
                    *error = QStringLiteral("Unexpected end of file while copying %1")
                                 .arg(entry.relativePath);
                return false;
            }
            if (hash)
                hash->addData(QByteArrayView(buffer.data(), read));

            ssize_t written = 0;
            while (written < read) {
                const ssize_t result = ::pwrite(output, buffer.data() + written,
                                                static_cast<size_t>(read - written),
                                                cursor + written);
                if (result < 0) {
                    if (errno == EINTR)
                        continue;
                    if (error)
                        *error = QStringLiteral("Cannot write %1: %2")
                                     .arg(entry.relativePath, errnoString());
                    return false;
                }
                written += result;
            }
            cursor += read;
        }
        position = hole;
        *supported = true;
        if (progress) {
            progress(static_cast<quint64>(baseBytes + static_cast<qint64>(position)),
                     entry.relativePath);
        }
    }
    return true;
}

bool copyRegularFile(const QString &source, const QString &destination, const FileEntry &entry,
                     const CopyEngine::ProgressFn &progress, qint64 *bytesCopied,
                     const CopyEngine::CancelFn &cancelled, const CopyEngine::PauseFn &paused,
                     std::vector<char> &buffer, QString *error,
                     CopyEngine::FileHash *hashOut = nullptr)
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

    QCryptographicHash sourceHash(QCryptographicHash::Sha256);
    const bool wantHash = hashOut != nullptr;
    if (hashOut)
        *hashOut = CopyEngine::FileHash{};

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

    bool sparseCopied = false;
    if (ok && !cloned) {
        struct stat inputStat {};
        if (::fstat(input, &inputStat) == 0 && S_ISREG(inputStat.st_mode)
            && static_cast<quint64>(inputStat.st_size) == entry.size
            && static_cast<quint64>(inputStat.st_blocks) * 512 < entry.size) {
            const qint64 baseBytes = *bytesCopied;
            bool supported = false;
            const bool copied = copySparseExtents(input, output, entry.size, entry, progress,
                                                  baseBytes, cancelled, paused, buffer, error,
                                                  &supported, wantHash ? &sourceHash : nullptr);
            if (copied && supported) {
                if (::ftruncate(output, static_cast<off_t>(entry.size)) != 0) {
                    if (error)
                        *error = QStringLiteral("Cannot size sparse destination %1: %2")
                                     .arg(temporary, errnoString());
                    ok = false;
                } else {
                    *bytesCopied += static_cast<qint64>(entry.size);
                    sparseCopied = true;
                }
            } else if (!supported && ok) {
                // A filesystem may reject SEEK_DATA after the clone attempt.
                // Remove any extents written before falling back to a normal copy.
                (void)::ftruncate(output, 0);
                (void)::lseek(output, 0, SEEK_SET);
                sourceHash.reset();
            }
        }
    }

    QElapsedTimer timer;
    timer.start();
    while (ok && !cloned && !sparseCopied) {
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

        if (wantHash)
            sourceHash.addData(QByteArrayView(buffer.data(), read));
        *bytesCopied += read;
        if (timer.elapsed() >= kProgressIntervalMs) {
            if (progress)
                progress(static_cast<quint64>(*bytesCopied), entry.relativePath);
            timer.restart();
        }
    }

    struct stat sourceStat {};
    bool sourceStatOk = false;
    if (hashOut && ok && !cloned)
        sourceStatOk = ::fstat(input, &sourceStat) == 0;

    // Game payloads are not read again until verification (and full
    // verification re-reads them deliberately), so release the source pages
    // instead of evicting the rest of the user's working set.
    if (ok && !cloned)
        (void)::posix_fadvise(input, 0, 0, POSIX_FADV_DONTNEED);

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
        struct stat destinationStat {};
        if (lstatPath(destination, &destinationStat) && isDirectory(destinationStat)) {
            if (error)
                *error = QStringLiteral("Refusing to replace a directory with a file: %1")
                             .arg(destination);
            ::unlink(temporaryName.constData());
            return false;
        }
        // rename(2) atomically replaces a file or symlink at the destination.
        // Removing the old path first would open a window where the game file is
        // missing if the process is interrupted between unlink and rename.
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
    if (hashOut && ok && !cloned) {
        hashOut->sha256 = sourceHash.result();
        if (sourceStatOk) {
            hashOut->size = static_cast<quint64>(sourceStat.st_size);
            hashOut->mtimeSec = sourceStat.st_mtim.tv_sec;
            hashOut->mtimeNsec = sourceStat.st_mtim.tv_nsec;
            hashOut->ctimeSec = sourceStat.st_ctim.tv_sec;
            hashOut->ctimeNsec = sourceStat.st_ctim.tv_nsec;
            hashOut->device = static_cast<quint64>(sourceStat.st_dev);
            hashOut->inode = static_cast<quint64>(sourceStat.st_ino);
        }
    }
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
              const CopyEngine::CancelFn &cancelled,
              const std::function<void(qint64)> &progress = {})
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
        if (progress)
            progress(static_cast<qint64>(chunk.size()));
    }
    if (cancelled && cancelled()) {
        if (error)
            *error = QStringLiteral("Cancelled by user");
        return false;
    }
    *result = hash.result();
    return true;
}

struct VerifyJob
{
    QString sourcePath;
    QString destinationPath;
    QString relativePath;
    quint64 size = 0;
    CopyEngine::FileHash cached;
    bool hasCached = false;
};

struct VerifyProgressState
{
    QElapsedTimer timer;
    std::atomic<quint64> bytes{0};
    std::atomic<int> cachedFiles{0};
    std::atomic<int> sourceFilesHashed{0};
    std::atomic_bool stop{false};
    std::mutex mutex;
    QString error;
    bool failed = false;

    void addBytes(qint64 count, const QString &path, const CopyEngine::ProgressFn &progress)
    {
        if (count <= 0)
            return;
        bytes.fetch_add(static_cast<quint64>(count), std::memory_order_relaxed);
        if (!progress)
            return;
        std::lock_guard<std::mutex> lock(mutex);
        if (timer.elapsed() < kProgressIntervalMs)
            return;
        progress(bytes.load(std::memory_order_relaxed), path);
        timer.restart();
    }

    void fail(const QString &message)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!failed) {
                failed = true;
                error = message;
            }
        }
        stop.store(true, std::memory_order_relaxed);
    }
};

bool trustedCachedHash(const QString &sourcePath, const FileEntry &entry,
                       const CopyEngine::HashLookup *sourceHashes,
                       CopyEngine::FileHash *cached)
{
    if (!sourceHashes || !cached)
        return false;
    const auto it = sourceHashes->constFind(entry.relativePath);
    if (it == sourceHashes->constEnd() || it->sha256.size() != 32
        || it->size != entry.size || it->mtimeSec != entry.mtimeSec
        || it->mtimeNsec != entry.mtimeNsec)
        return false;

    struct stat current {};
    if (::lstat(QFile::encodeName(sourcePath).constData(), &current) != 0
        || !S_ISREG(current.st_mode)
        || static_cast<quint64>(current.st_size) != it->size
        || current.st_mtim.tv_sec != it->mtimeSec
        || current.st_mtim.tv_nsec != it->mtimeNsec
        || current.st_ctim.tv_sec != it->ctimeSec
        || current.st_ctim.tv_nsec != it->ctimeNsec
        || static_cast<quint64>(current.st_dev) != it->device
        || static_cast<quint64>(current.st_ino) != it->inode) {
        return false;
    }
    *cached = it.value();
    return true;
}

bool verifyHashedFile(const VerifyJob &job, VerifyProgressState &state,
                      const CopyEngine::CancelFn &cancelled,
                      const CopyEngine::ProgressFn &progress)
{
    const auto destProgress = [&](qint64 bytes) {
        state.addBytes(bytes, job.relativePath, progress);
    };
    QString hashError;
    QByteArray destinationHash;
    if (job.hasCached) {
        state.cachedFiles.fetch_add(1, std::memory_order_relaxed);
        if (!hashFile(job.destinationPath, &destinationHash, &hashError, cancelled, destProgress)) {
            state.fail(hashError);
            return false;
        }
        if (destinationHash != job.cached.sha256) {
            state.fail(QStringLiteral("Checksum mismatch: %1").arg(job.relativePath));
            return false;
        }
        return true;
    }

    state.sourceFilesHashed.fetch_add(1, std::memory_order_relaxed);
    QByteArray sourceHash;
    if (!hashFile(job.sourcePath, &sourceHash, &hashError, cancelled, {})) {
        state.fail(hashError);
        return false;
    }
    if (!hashFile(job.destinationPath, &destinationHash, &hashError, cancelled, destProgress)) {
        state.fail(hashError);
        return false;
    }
    if (sourceHash != destinationHash) {
        state.fail(QStringLiteral("Checksum mismatch: %1").arg(job.relativePath));
        return false;
    }
    return true;
}

bool runVerifyJobs(const QVector<VerifyJob> &jobs, bool fullHash,
                   const CopyEngine::CancelFn &cancelled,
                   const CopyEngine::ProgressFn &progress,
                   CopyEngine::VerifyStats *stats, QString *error)
{
    if (stats)
        *stats = CopyEngine::VerifyStats{};
    if (!fullHash)
        return true;

    quint64 totalBytes = 0;
    for (const VerifyJob &job : jobs)
        totalBytes += job.size;
    if (jobs.isEmpty()) {
        if (progress)
            progress(0, QString());
        return true;
    }

    VerifyProgressState state;
    state.timer.start();
    unsigned workerCount = 1;
    if (jobs.size() >= 4 && totalBytes >= 4ull * 1024 * 1024) {
        workerCount = std::thread::hardware_concurrency();
        if (workerCount == 0)
            workerCount = 1;
        workerCount = std::min<unsigned>(workerCount, 4);
        workerCount = std::min<unsigned>(workerCount, static_cast<unsigned>(jobs.size()));
    }

    std::atomic<size_t> nextIndex{0};
    const auto worker = [&]() {
        while (!state.stop.load(std::memory_order_relaxed)) {
            if (cancelled && cancelled()) {
                state.fail(QStringLiteral("Cancelled by user"));
                return;
            }
            const size_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
            if (index >= static_cast<size_t>(jobs.size()))
                return;
            if (!verifyHashedFile(jobs.at(static_cast<int>(index)), state, cancelled, progress))
                return;
        }
    };

    if (workerCount <= 1) {
        worker();
    } else {
        std::vector<std::thread> threads;
        threads.reserve(workerCount);
        for (unsigned i = 0; i < workerCount; ++i)
            threads.emplace_back(worker);
        for (std::thread &thread : threads)
            thread.join();
    }

    if (stats) {
        stats->cachedFiles = state.cachedFiles.load(std::memory_order_relaxed);
        stats->sourceFilesHashed = state.sourceFilesHashed.load(std::memory_order_relaxed);
    }
    if (state.failed) {
        if (error)
            *error = state.error;
        return false;
    }
    if (progress)
        progress(state.bytes.load(std::memory_order_relaxed), QString());
    return true;
}

} // namespace

bool CopyEngine::copyTree(const QString &source, const QString &destination,
                          const ProgressFn &progress, const CancelFn &cancelled,
                          qint64 *bytesCopiedOut, QString *error, const PauseFn &paused,
                          HashLookup *sourceHashes)
{
    if (sourceHashes)
        sourceHashes->clear();
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
            const QString temporary = destPath + QStringLiteral(".slm-link-")
                + QString::number(::getpid()) + QLatin1Char('-')
                + QString::number(static_cast<qulonglong>(QDateTime::currentMSecsSinceEpoch()));
            const QByteArray temporaryName = QFile::encodeName(temporary);
            if (::symlink(entry.symlinkTarget.toUtf8().constData(), temporaryName.constData()) != 0) {
                if (error)
                    *error = QStringLiteral("Cannot create symlink %1: %2").arg(destPath, errnoString());
                ok = false;
                break;
            }
            applyTimesAndMode(temporary, entry, true);
            if (::rename(temporaryName.constData(), QFile::encodeName(destPath).constData()) != 0) {
                if (error)
                    *error = QStringLiteral("Cannot finalize symlink %1: %2")
                                 .arg(destPath, errnoString());
                ::unlink(temporaryName.constData());
                ok = false;
                break;
            }
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
                if (lstatPath(destPath, &st) && S_ISDIR(st.st_mode)) {
                    if (error)
                        *error = QStringLiteral("Refusing to replace directory %1").arg(destPath);
                    ok = false;
                    break;
                }
                const QString temporary = destPath + QStringLiteral(".slm-link-")
                    + QString::number(::getpid()) + QLatin1Char('-')
                    + QString::number(static_cast<qulonglong>(QDateTime::currentMSecsSinceEpoch()));
                const QByteArray temporaryName = QFile::encodeName(temporary);
                if (::link(QFile::encodeName(existing).constData(), temporaryName.constData()) != 0) {
                    if (error)
                        *error = QStringLiteral("Cannot create hard link %1: %2")
                                     .arg(destPath, errnoString());
                    ok = false;
                    break;
                }
                if (::rename(temporaryName.constData(), QFile::encodeName(destPath).constData()) != 0) {
                    if (error)
                        *error = QStringLiteral("Cannot finalize hard link %1: %2")
                                     .arg(destPath, errnoString());
                    ::unlink(temporaryName.constData());
                    ok = false;
                    break;
                }
                break;
            }

            FileHash sourceHash;
            if (!copyRegularFile(source + QLatin1Char('/') + entry.relativePath, destPath, entry,
                                 progress, &bytesCopied, cancelled, paused, copyBuffer, error,
                                 sourceHashes ? &sourceHash : nullptr)) {
                ok = false;
                break;
            }
            if (sourceHashes && !sourceHash.sha256.isEmpty())
                sourceHashes->insert(entry.relativePath, sourceHash);
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
                            QString *error, const CancelFn &cancelled, const ProgressFn &progress,
                            const HashLookup *sourceHashes, VerifyStats *stats)
{
    if (stats)
        *stats = VerifyStats{};
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

    QVector<VerifyJob> hashJobs;

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
                VerifyJob job;
                job.sourcePath = source + QLatin1Char('/') + sourceEntry.relativePath;
                job.destinationPath = destination + QLatin1Char('/') + destEntry.relativePath;
                job.relativePath = sourceEntry.relativePath;
                job.size = sourceEntry.size;
                job.hasCached = trustedCachedHash(job.sourcePath, sourceEntry, sourceHashes,
                                                  &job.cached);
                hashJobs.append(job);
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
    return runVerifyJobs(hashJobs, fullHash, cancelled, progress, stats, error);
}

} // namespace slm
