// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.h"

#include <QString>
#include <QVector>

namespace slm {

enum class HealthSeverity { Info, Warning, Critical };

enum class HealthKind {
    MissingGamePayload,
    OrphanGameFolder,
    DuplicateAppId,
    StaleTempManifest,
    StaleLockFile,
    BrokenCompatData,
    CompatDataOnWindowsFs,
    OrphanShaderCache,
    OrphanWorkshopContent,
    OrphanPrefix,
    SourceBackup,
};

struct HealthFinding
{
    HealthSeverity severity = HealthSeverity::Info;
    HealthKind kind = HealthKind::OrphanGameFolder;
    QString libraryPath;
    QString path;
    QString title;
    QString detail;
    bool quarantineable = false;
};

class LibraryHealth
{
public:
    static QVector<HealthFinding> scan(const SteamScan &scan, QString *error = nullptr);
    static bool quarantine(const SteamScan &scan, const QVector<HealthFinding> &findings,
                           QString *error = nullptr);
    static QString kindName(HealthKind kind);
    static QString severityName(HealthSeverity severity);
};

} // namespace slm
