// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mountinfo.h"
#include "types.h"
#include "vdf.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace slm {

struct LibraryLoadResult
{
    bool ok = false;
    QString error;
    QString vdfPath;
    QByteArray vdfRaw;
    QVector<Library> libraries;
    QVector<Issue> issues;
};

LibraryLoadResult loadLibraries(const SteamRoot &root, const MountTable &mounts);

class LibraryFoldersFile
{
public:
    bool load(const QString &path, QString *error = nullptr);
    bool reload(QString *error = nullptr);
    bool save(QString *error = nullptr) const;
    bool moveApp(quint32 appId, const QString &sourcePath, const QString &destPath, quint64 size,
                 QString *error = nullptr);

    QByteArray raw() const;
    QByteArray hash() const;
    QString path() const { return m_path; }
    const VdfDocument &document() const { return m_document; }
    VdfDocument &document() { return m_document; }

private:
    VdfNode *libraryObject(const QString &path);
    static bool pathMatches(const VdfNode &object, const QString &path);

    QString m_path;
    QByteArray m_raw;
    VdfDocument m_document;
    bool m_loaded = false;
};

CompatDataInfo inspectCompatData(const Library &library, const MountTable &mounts);
QString resolvePrefixPath(const Library &library, quint32 appId, bool *exists = nullptr);

} // namespace slm
