// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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

    static bool copyTree(const QString &source, const QString &destination, const ProgressFn &progress,
                         const CancelFn &cancelled, qint64 *bytesCopied = nullptr,
                         QString *error = nullptr, const PauseFn &paused = {});

    static bool verifyTree(const QString &source, const QString &destination, bool fullHash,
                           QString *error = nullptr, const CancelFn &cancelled = {});
};

} // namespace slm
