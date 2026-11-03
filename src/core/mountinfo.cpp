// SPDX-License-Identifier: GPL-3.0-or-later
#include "mountinfo.h"

#include "fileops.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>

#include <sys/stat.h>
#include <sys/statvfs.h>

namespace slm {

FsClass classifyFilesystem(const QString &fsType)
{
    const QString type = fsType.toLower();
    if (type == QStringLiteral("ntfs") || type == QStringLiteral("ntfs3")
        || type == QStringLiteral("fuseblk")) {
        return FsClass::Ntfs;
    }
    if (type == QStringLiteral("exfat"))
        return FsClass::Exfat;
    if (type == QStringLiteral("vfat") || type == QStringLiteral("fat") || type == QStringLiteral("msdos")
        || type == QStringLiteral("fat32")) {
        return FsClass::Fat32;
    }
    if (type == QStringLiteral("ext2") || type == QStringLiteral("ext3")
        || type == QStringLiteral("ext4") || type == QStringLiteral("btrfs")
        || type == QStringLiteral("xfs") || type == QStringLiteral("f2fs")
        || type == QStringLiteral("zfs") || type == QStringLiteral("reiserfs")
        || type == QStringLiteral("jfs") || type == QStringLiteral("nilfs2")
        || type == QStringLiteral("bcachefs") || type == QStringLiteral("tmpfs")
        || type == QStringLiteral("overlay") || type == QStringLiteral("aufs")) {
        return FsClass::Posix;
    }
    if (type == QStringLiteral("nfs") || type == QStringLiteral("nfs4")
        || type == QStringLiteral("cifs") || type == QStringLiteral("smbfs")
        || type == QStringLiteral("sshfs") || type == QStringLiteral("fuse.sshfs")
        || type == QStringLiteral("9p") || type == QStringLiteral("afs")
        || type == QStringLiteral("ceph") || type == QStringLiteral("glusterfs")) {
        return FsClass::Network;
    }
    return FsClass::Other;
}

bool isWindowsCompatibleClass(FsClass fsClass)
{
    return fsClass == FsClass::Ntfs || fsClass == FsClass::Exfat || fsClass == FsClass::Fat32;
}

bool isNetworkClass(FsClass fsClass)
{
    return fsClass == FsClass::Network;
}

MountInfo MountTable::makeMount(const QString &mountPoint, const QString &fsType, const QString &source,
                                const QStringList &options)
{
    MountInfo info;
    info.mountPoint = mountPoint;
    info.fsType = fsType;
    info.source = source;
    info.options = options;
    info.fsClass = classifyFilesystem(fsType);
    info.readOnly = options.contains(QStringLiteral("ro"), Qt::CaseInsensitive);

    const QByteArray encoded = QFile::encodeName(mountPoint);
    struct stat st {};
    if (::stat(encoded.constData(), &st) == 0)
        info.device = st.st_dev;

    struct statvfs vfs {};
    if (::statvfs(encoded.constData(), &vfs) == 0) {
        info.totalBytes = static_cast<quint64>(vfs.f_blocks) * vfs.f_frsize;
        info.freeBytes = static_cast<quint64>(vfs.f_bavail) * vfs.f_frsize;
    }
    return info;
}

QVector<MountInfo> MountTable::parseMountInfo(const QByteArray &data, QString *error)
{
    QVector<MountInfo> result;
    const QList<QByteArray> lines = data.split('\n');
    for (const QByteArray &rawLine : lines) {
        const QByteArray line = rawLine.trimmed();
        if (line.isEmpty())
            continue;

        const int separator = line.indexOf(" - ");
        if (separator < 0)
            continue;

        const QList<QByteArray> before = line.left(separator).split(' ');
        const QList<QByteArray> after = line.mid(separator + 3).split(' ');
        if (before.size() < 6 || after.size() < 3)
            continue;

        const QString mountPoint = decodeMountInfoEscapes(before.at(4));
        const QStringList mountOptions =
            QString::fromLatin1(before.at(5)).split(',', Qt::SkipEmptyParts);
        const QString fsType = QString::fromLatin1(after.at(0));
        const QString source = decodeMountInfoEscapes(after.at(1));
        const QStringList superOptions =
            QString::fromLatin1(after.at(2)).split(',', Qt::SkipEmptyParts);

        MountInfo info = makeMount(mountPoint, fsType, source, mountOptions + superOptions);
        bool idOk = false;
        info.mountId = before.at(0).toULongLong(&idOk);
        result.append(info);
    }

    std::sort(result.begin(), result.end(), [](const MountInfo &a, const MountInfo &b) {
        return a.mountPoint.size() > b.mountPoint.size();
    });

    if (error)
        error->clear();
    return result;
}

MountTable MountTable::load(QString *error)
{
    MountTable table;
    QFile file(QStringLiteral("/proc/self/mountinfo"));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot read /proc/self/mountinfo: %1").arg(file.errorString());
        return table;
    }
    const QByteArray data = file.readAll();
    table.m_mounts = parseMountInfo(data, error);
    table.m_valid = !table.m_mounts.isEmpty();
    if (!table.m_valid && error && error->isEmpty())
        *error = QStringLiteral("No mount entries found");
    return table;
}

MountTable MountTable::fromMounts(const QVector<MountInfo> &mounts)
{
    MountTable table;
    table.m_mounts = mounts;
    std::sort(table.m_mounts.begin(), table.m_mounts.end(), [](const MountInfo &a, const MountInfo &b) {
        return a.mountPoint.size() > b.mountPoint.size();
    });
    table.m_valid = !table.m_mounts.isEmpty();
    return table;
}

MountInfo MountTable::forPath(const QString &path) const
{
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (const MountInfo &mount : m_mounts) {
        if (mount.mountPoint == QStringLiteral("/"))
            continue;
        if (absolute == mount.mountPoint
            || absolute.startsWith(mount.mountPoint + QLatin1Char('/'))) {
            return mount;
        }
    }
    for (const MountInfo &mount : m_mounts) {
        if (mount.mountPoint == QStringLiteral("/"))
            return mount;
    }
    return {};
}

} // namespace slm
