// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/types.h"

#include <QColor>
#include <QIcon>
#include <QString>

namespace ui {

enum LibraryItemRole {
    SubtitleRole = Qt::UserRole + 20,
    UsagePercentRole,
    QueuedBytesRole,
    TypeColorRole,
};

QString filesystemLabel(const slm::MountInfo &mount);
QString driveTypeLabel(const slm::MountInfo &mount);
QIcon driveIcon(const slm::Library &library);
QColor driveTypeColor(const slm::Library &library);
QIcon allLibrariesIcon();
QString driveDisplayName(const slm::Library &library);
QString driveSubtitle(const slm::Library &library, int gameCount = -1);
QString driveTooltip(const slm::Library &library);

} // namespace ui
