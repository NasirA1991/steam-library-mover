// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.h"

#include <QStringList>
#include <QVector>

#include <functional>

namespace slm {

struct ProcessUsage
{
    bool steamRunning = false;
    QVector<qint64> steamPids;
    QStringList steamNames;
    bool prefixInUse = false;
    QStringList prefixUsers;
    bool gameInUse = false;
    QStringList gameUsers;
    QStringList warnings;
};

ProcessUsage inspectProcesses(const QStringList &prefixes, const QStringList &gameDirs);

// Test-only seam. Production code never installs an inspector.
using ProcessInspector = std::function<ProcessUsage(const QStringList &, const QStringList &)>;
void setProcessInspectorForTesting(ProcessInspector inspector);
bool requestSteamShutdown(InstallKind kind, QString *error = nullptr);
QString steamShutdownCommand(InstallKind kind);
QString steamLaunchCommand(InstallKind kind);
bool startSteam(InstallKind kind, QString *error = nullptr);

bool isSteamProcessName(const QString &name);

} // namespace slm
