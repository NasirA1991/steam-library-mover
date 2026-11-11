// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QtGlobal>

#include <functional>

namespace slm {

class CopyEngine
{
public:
    using ProgressFn = std::function<void(quint64 bytesInTree, const QString &currentRelativePath)>;
    using CancelFn = std::function<bool()>;
    using PauseFn = std::function<bool()>;

    struct FileHash
    {
        QByteArray sha256;
        quint64 size = 0;
        qint64 mtimeSec = 0;
        qint64 mtimeNsec = 0;
        qint64 ctimeSec = 0;
        qint64 ctimeNsec = 0;
        quint64 device = 0;
        quint64 inode = 0;
    };
    using HashLookup = QHash<QString, FileHash>;

    struct VerifyStats
    {
        int sourceFilesHashed = 0;
        int cachedFiles = 0;
    };

    static bool copyTree(const QString &source, const QString &destination, const ProgressFn &progress,
                         const CancelFn &cancelled, qint64 *bytesCopied = nullptr,
                         QString *error = nullptr, const PauseFn &paused = {},
                         HashLookup *sourceHashes = nullptr);

    // Full verification may hash independent files in parallel. The progress
    // callback is serialized; callers must not assume it runs on their thread.
    static bool verifyTree(const QString &source, const QString &destination, bool fullHash,
                           QString *error = nullptr, const CancelFn &cancelled = {},
                           const ProgressFn &progress = {},
                           const HashLookup *sourceHashes = nullptr,
                           VerifyStats *stats = nullptr);
};

} // namespace slm
