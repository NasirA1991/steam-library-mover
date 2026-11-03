// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.h"

#include <QVector>

namespace slm {

class MountTable
{
public:
    static MountTable load(QString *error = nullptr);
    static MountTable fromMounts(const QVector<MountInfo> &mounts);
    static QVector<MountInfo> parseMountInfo(const QByteArray &data, QString *error = nullptr);

    MountInfo forPath(const QString &path) const;
    QVector<MountInfo> mounts() const { return m_mounts; }
    bool isValid() const { return m_valid; }

private:
    static MountInfo makeMount(const QString &mountPoint, const QString &fsType, const QString &source,
                               const QStringList &options);
    QVector<MountInfo> m_mounts;
    bool m_valid = false;
};

FsClass classifyFilesystem(const QString &fsType);
bool isWindowsCompatibleClass(FsClass fsClass);
bool isNetworkClass(FsClass fsClass);

} // namespace slm
