// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mountinfo.h"
#include "types.h"

#include <QJsonObject>
#include <QList>
#include <QString>

#include <functional>

namespace slm {

class MoveExecutor
{
public:
    using ProgressFn = std::function<void(const MoveProgress &)>;
    using CancelFn = std::function<bool()>;
    using PauseFn = std::function<bool()>;

    MoveResult execute(const MovePlan &plan, const MountTable &mounts, const ProgressFn &progress = {},
                       const CancelFn &cancelled = {}, const PauseFn &paused = {},
                       const QJsonObject &resumeJournal = {});

    // Rebuild a plan from a journal so an interrupted copy can be resumed.
    static MovePlan planFromJournal(const QJsonObject &journal, QString *error = nullptr);
    static bool canResume(const QJsonObject &journal, QString *reason = nullptr);
    MoveResult resumeOperation(const QJsonObject &journal, const MountTable &mounts,
                               const ProgressFn &progress = {}, const CancelFn &cancelled = {},
                               const PauseFn &paused = {});

    static QList<QJsonObject> unfinishedOperations();
    static bool rollbackOperation(const QJsonObject &journal, QString *error = nullptr);
};

} // namespace slm
