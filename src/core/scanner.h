// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mountinfo.h"
#include "types.h"

#include <QStringList>
#include <QVector>

namespace slm {

QVector<SteamRoot> detectSteamRoots(const QString &overridePath = {}, QStringList *notes = nullptr);
SteamScan scanSteam(const SteamRoot &root, const MountTable &mounts, QString *error = nullptr);

} // namespace slm
