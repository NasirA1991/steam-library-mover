// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mountinfo.h"
#include "types.h"

#include <QVector>

namespace slm {

class NtfsDoctor
{
public:
    static QVector<Issue> inspect(const SteamScan &scan);
    static QString defaultCompatDataTarget(const SteamScan &scan);
    static bool fixCompatData(const Library &library, const QString &target, const MountTable &mounts,
                              bool apply, QString *error = nullptr);
};

} // namespace slm
