// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mountinfo.h"
#include "types.h"

#include <QString>

namespace slm {

class MovePlanner
{
public:
    static MovePlan plan(const SteamScan &scan, quint32 appId, const QString &destinationLibrary,
                         const MoveOptions &options, const MountTable &mounts,
                         QString *error = nullptr,
                         const QMap<QString, quint64> &reservedBytes = {});

    static bool revalidate(const MovePlan &plan, const MountTable &mounts, QString *error = nullptr);
    static bool refreshVdfSnapshot(MovePlan &plan, QString *error = nullptr);

    static QStringList processPrefixPaths(const SteamScan &scan, const Game &game,
                                          const Library &destLibrary);
};

} // namespace slm
