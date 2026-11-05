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

bool removeTree(const QString &path, QString *error)
{
    struct stat st {};
    const QByteArray encodedPath = QFile::encodeName(path);
    if (::lstat(encodedPath.constData(), &st) != 0) {
        if (errno == ENOENT)
            return true;
        if (error)
            *error = QStringLiteral("Cannot inspect %1: %2").arg(path, errnoString());
        return false;
    }

    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        if (::unlink(encodedPath.constData()) != 0) {
            if (error)
                *error = QStringLiteral("Cannot remove %1: %2").arg(path, errnoString());
            return false;
        }
        return true;
    }

    DIR *dir = ::opendir(encodedPath.constData());
    if (!dir) {
        if (error)
            *error = QStringLiteral("Cannot open directory %1: %2").arg(path, errnoString());
        return false;
    }

    bool ok = true;
    QString firstError;
    while (dirent *entry = ::readdir(dir)) {
        const QByteArray name(entry->d_name);
        if (name == "." || name == "..")
            continue;
        QString childError;
        if (!removeTree(path + QLatin1Char('/') + QFile::decodeName(name), &childError)) {
            if (ok) {
                ok = false;
                firstError = childError;
            }
        }
    }
    ::closedir(dir);

    if (ok && ::rmdir(encodedPath.constData()) != 0) {
        firstError = QStringLiteral("Cannot remove directory %1: %2").arg(path, errnoString());
        ok = false;
    }
    if (!ok && error)
        *error = firstError;
    return ok;
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
