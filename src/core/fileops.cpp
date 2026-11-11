// SPDX-License-Identifier: GPL-3.0-or-later
#include "fileops.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>

#include <cerrno>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace slm::fsutil {

namespace {

QString errnoString()
{
    return QString::fromLocal8Bit(::strerror(errno));
}

} // namespace

bool writeFileAtomic(const QString &path, const QByteArray &data, int mode, QString *error)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Cannot open %1 for writing: %2").arg(path, file.errorString());
        return false;
    }
    if (file.write(data) != data.size()) {
        if (error)
            *error = QStringLiteral("Cannot write %1: %2").arg(path, file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("Cannot commit %1: %2").arg(path, file.errorString());
        return false;
    }
    if (mode >= 0) {
        // Modes are best-effort: NTFS/FUSE destinations may not support chmod.
        ::chmod(QFile::encodeName(path).constData(), static_cast<mode_t>(mode));
    }
    return fsyncDir(QFileInfo(path).absolutePath(), error);
}

bool fsyncDir(const QString &dir, QString *error)
{
    const QByteArray encoded = QFile::encodeName(dir);
    const int fd = ::open(encoded.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        if (error)
            *error = QStringLiteral("Cannot open directory %1: %2").arg(dir, errnoString());
        return false;
    }
    const int rc = ::fsync(fd);
    const int savedErrno = errno;
    ::close(fd);
    if (rc != 0 && savedErrno != EINVAL && savedErrno != ENOTSUP) {
        if (error)
            *error = QStringLiteral("Cannot fsync directory %1: %2").arg(dir, errnoString());
        return false;
    }
    return true;
}

bool syncFilesystem(const QString &path, QString *error)
{
    const QByteArray encoded = QFile::encodeName(path);
    const int fd = ::open(encoded.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        if (error)
            *error = QStringLiteral("Cannot open %1 for sync: %2").arg(path, errnoString());
        return false;
    }
    errno = 0;
    const int rc = ::syncfs(fd);
    const int savedErrno = errno;
    ::close(fd);
    if (rc == 0)
        return true;
    if (savedErrno == EINVAL || savedErrno == ENOSYS || savedErrno == ENOTSUP) {
        ::sync();
        return true;
    }
    if (error)
        *error = QStringLiteral("Cannot sync filesystem containing %1: %2")
                     .arg(path, QString::fromLocal8Bit(::strerror(savedErrno)));
    return false;
}

bool ensureDir(const QString &path, QString *error)
{
    if (path.isEmpty()) {
        if (error)
            *error = QStringLiteral("Empty directory path");
        return false;
    }
    const QFileInfo info(path);
    if (info.exists()) {
        if (info.isDir())
            return true;
        if (error)
            *error = QStringLiteral("%1 exists but is not a directory").arg(path);
        return false;
    }
    if (!QDir().mkpath(path)) {
        if (error)
            *error = QStringLiteral("Cannot create directory %1").arg(path);
        return false;
    }
    return true;
}

namespace {

bool removeTreeFd(int fd, const QString &display, dev_t rootDevice, int depth, QString *error);

bool unlinkAtPath(int parentFd, const QByteArray &name, const QString &display, QString *error)
{
    if (::unlinkat(parentFd, name.constData(), 0) == 0 || errno == ENOENT)
        return true;
    if (error)
        *error = QStringLiteral("Cannot remove %1: %2").arg(display, errnoString());
    return false;
}

bool removeTreeFd(int fd, const QString &display, dev_t rootDevice, int depth, QString *error)
{
    if (depth > 256) {
        ::close(fd);
        if (error)
            *error = QStringLiteral("Directory tree is too deep at %1").arg(display);
        return false;
    }

    DIR *dir = ::fdopendir(fd);
    if (!dir) {
        const int savedErrno = errno;
        ::close(fd);
        if (error)
            *error = QStringLiteral("Cannot open directory %1: %2")
                         .arg(display, QString::fromLocal8Bit(::strerror(savedErrno)));
        return false;
    }
    const int dirFd = ::dirfd(dir);
    bool ok = true;
    QString firstError;

    while (const dirent *entry = ::readdir(dir)) {
        const QByteArray name(entry->d_name);
        if (name == "." || name == "..")
            continue;
        const QString childDisplay = display + QLatin1Char('/') + QFile::decodeName(name);
        struct stat st {};
        if (::fstatat(dirFd, name.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT)
                continue;
            if (ok) {
                ok = false;
                firstError = QStringLiteral("Cannot inspect %1: %2")
                                 .arg(childDisplay, errnoString());
            }
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (st.st_dev != rootDevice) {
                if (ok) {
                    ok = false;
                    firstError = QStringLiteral(
                        "Refusing to remove %1 across a filesystem boundary").arg(childDisplay);
                }
                continue;
            }
            // O_NOFOLLOW makes a symlink swap fail instead of redirecting the
            // recursive delete outside this directory.
            const int childFd = ::openat(dirFd, name.constData(),
                                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (childFd < 0) {
                if (errno == ENOENT)
                    continue;
                if (errno == ELOOP || errno == ENOTDIR) {
                    // The directory was replaced by a symlink or file after
                    // fstatat(); unlink the entry itself, never its target.
                    if (!unlinkAtPath(dirFd, name, childDisplay, &firstError))
                        ok = false;
                    continue;
                }
                if (ok) {
                    ok = false;
                    firstError = QStringLiteral("Cannot open directory %1: %2")
                                     .arg(childDisplay, errnoString());
                }
                continue;
            }
            const bool childOk = removeTreeFd(childFd, childDisplay, rootDevice, depth + 1,
                                              ok ? nullptr : &firstError);
            if (!childOk) {
                ok = false;
                continue;
            }
            if (::unlinkat(dirFd, name.constData(), AT_REMOVEDIR) != 0 && errno != ENOENT) {
                if (ok)
                    firstError = QStringLiteral("Cannot remove directory %1: %2")
                                     .arg(childDisplay, errnoString());
                ok = false;
            }
        } else if (!unlinkAtPath(dirFd, name, childDisplay, ok ? nullptr : &firstError)) {
            ok = false;
        }
    }
    ::closedir(dir);
    if (!ok && error)
        *error = firstError;
    return ok;
}

} // namespace

bool removeTree(const QString &path, QString *error)
{
    const QFileInfo info(path);
    const QString base = info.fileName();
    if (path.isEmpty() || base.isEmpty() || base == QLatin1String(".")
        || base == QLatin1String("..")) {
        if (error)
            *error = QStringLiteral("Refusing to remove unsafe path %1").arg(path);
        return false;
    }
    const QString parentPath = info.absolutePath();
    const QByteArray baseName = QFile::encodeName(base);
    const QByteArray parentEncoded = QFile::encodeName(parentPath);

    // Anchor the whole operation at the parent descriptor. If the final path
    // component is swapped for a symlink after this point, openat(O_NOFOLLOW)
    // refuses to follow it and unlinkat only removes the entry itself.
    const int parentFd = ::open(parentEncoded.constData(),
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parentFd < 0) {
        if (errno == ENOENT)
            return true;
        if (error)
            *error = QStringLiteral("Cannot open parent of %1: %2").arg(path, errnoString());
        return false;
    }

    const int fd = ::openat(parentFd, baseName.constData(),
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        const int savedErrno = errno;
        if (savedErrno == ENOENT) {
            ::close(parentFd);
            return true;
        }
        if (savedErrno == ELOOP || savedErrno == ENOTDIR) {
            // A symlink or regular file: remove the entry, never its target.
            const int rc = ::unlinkat(parentFd, baseName.constData(), 0);
            const int unlinkErrno = errno;
            ::close(parentFd);
            if (rc == 0 || unlinkErrno == ENOENT)
                return true;
            if (error)
                *error = QStringLiteral("Cannot remove %1: %2")
                             .arg(path, QString::fromLocal8Bit(::strerror(unlinkErrno)));
            return false;
        }
        ::close(parentFd);
        if (error)
            *error = QStringLiteral("Cannot open %1: %2")
                         .arg(path, QString::fromLocal8Bit(::strerror(savedErrno)));
        return false;
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int savedErrno = errno;
        ::close(fd);
        ::close(parentFd);
        if (error)
            *error = QStringLiteral("Cannot inspect %1: %2")
                         .arg(path, QString::fromLocal8Bit(::strerror(savedErrno)));
        return false;
    }

    if (!removeTreeFd(fd, path, st.st_dev, 0, error)) {
        ::close(parentFd);
        return false;
    }

    const int rc = ::unlinkat(parentFd, baseName.constData(), AT_REMOVEDIR);
    const int savedErrno = errno;
    ::close(parentFd);
    if (rc != 0 && savedErrno != ENOENT) {
        if (error)
            *error = QStringLiteral("Cannot remove directory %1: %2")
                         .arg(path, QString::fromLocal8Bit(::strerror(savedErrno)));
        return false;
    }
    return true;
}

std::optional<QByteArray> readSymlinkTarget(const QString &path, QString *error)
{
    QByteArray target(256, '\0');
    const QByteArray encoded = QFile::encodeName(path);
    for (;;) {
        const ssize_t length = ::readlink(encoded.constData(), target.data(), target.size());
        if (length < 0) {
            if (error)
                *error = QStringLiteral("Cannot read symlink %1: %2").arg(path, errnoString());
            return std::nullopt;
        }
        if (length < target.size()) {
            target.resize(static_cast<int>(length));
            return target;
        }
        target.resize(target.size() * 2);
    }
}

bool isSameFilesystem(const QString &a, const QString &b)
{
    struct stat stA {};
    struct stat stB {};
    if (::stat(QFile::encodeName(a).constData(), &stA) != 0)
        return false;
    if (::stat(QFile::encodeName(b).constData(), &stB) != 0)
        return false;
    return stA.st_dev == stB.st_dev;
}

QString uniqueSuffix()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

std::optional<QByteArray> readFile(const QString &path, qint64 maxSize, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return std::nullopt;
    }
    if (file.size() > maxSize) {
        if (error)
            *error = QStringLiteral("%1 is too large").arg(path);
        return std::nullopt;
    }
    return file.readAll();
}

} // namespace slm::fsutil
