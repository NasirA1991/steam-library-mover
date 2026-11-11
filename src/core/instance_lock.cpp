// SPDX-License-Identifier: GPL-3.0-or-later
#include "instance_lock.h"

#include <QFile>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace slm {

namespace {

QString errorText()
{
    return QString::fromLocal8Bit(::strerror(errno));
}

bool tryPath(const QString &path, int *fd, QString *error, bool *permissionProblem)
{
    const QByteArray encoded = QFile::encodeName(path);
    const int opened = ::open(encoded.constData(),
                              O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                              static_cast<mode_t>(0666));
    if (opened < 0) {
        if (permissionProblem)
            *permissionProblem = (errno == EACCES || errno == EPERM || errno == EROFS
                                  || errno == ENOENT);
        if (error)
            *error = QStringLiteral("Cannot open lock file %1: %2").arg(path, errorText());
        return false;
    }

    struct stat st {};
    if (::fstat(opened, &st) != 0 || !S_ISREG(st.st_mode)) {
        const QString message = QStringLiteral("Lock path is not a regular file: %1").arg(path);
        ::close(opened);
        if (error)
            *error = message;
        return false;
    }
    // The first creator controls the mode. Make the lock usable by other users
    // on the machine; failure here is non-fatal (the lock still works locally).
    if (st.st_uid == ::geteuid())
        (void)::fchmod(opened, static_cast<mode_t>(0666));

    if (::flock(opened, LOCK_EX | LOCK_NB) != 0) {
        const int savedErrno = errno;
        ::close(opened);
        if (savedErrno == EWOULDBLOCK || savedErrno == EAGAIN) {
            if (error)
                *error = QStringLiteral("Steam Library Mover is already running");
        } else if (error) {
            *error = QStringLiteral("Cannot lock %1: %2")
                         .arg(path, QString::fromLocal8Bit(::strerror(savedErrno)));
        }
        return false;
    }

    *fd = opened;
    return true;
}

} // namespace

InstanceLock::~InstanceLock()
{
    release();
}

void InstanceLock::release()
{
    if (m_fd >= 0) {
        (void)::flock(m_fd, LOCK_UN);
        ::close(m_fd);
        m_fd = -1;
    }
    m_path.clear();
}

bool InstanceLock::openAndLock(const QString &path, QString *error)
{
    bool permissionProblem = false;
    if (!tryPath(path, &m_fd, error, &permissionProblem)) {
        if (permissionProblem) {
            // The caller may fall back to another location only for a missing or
            // unwritable system lock directory, never for a held lock.
            return false;
        }
        return false;
    }
    m_path = path;
    return true;
}

bool InstanceLock::acquire(QString *error, const QString &path)
{
    release();
    if (!path.isEmpty())
        return openAndLock(path, error);

    QString firstError;
    bool permissionProblem = false;
    if (!tryPath(QStringLiteral("/run/lock/steam-library-mover.lock"), &m_fd, &firstError,
                 &permissionProblem)) {
        if (!permissionProblem) {
            if (error)
                *error = firstError;
            return false;
        }
        // Most desktop users cannot write /run/lock. /tmp is world-writable and
        // the advisory flock is still machine-wide.
        QString secondError;
        if (!tryPath(QStringLiteral("/tmp/steam-library-mover.lock"), &m_fd, &secondError,
                     nullptr)) {
            if (error)
                *error = secondError;
            return false;
        }
        m_path = QStringLiteral("/tmp/steam-library-mover.lock");
        return true;
    }
    m_path = QStringLiteral("/run/lock/steam-library-mover.lock");
    return true;
}

} // namespace slm
