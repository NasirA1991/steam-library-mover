// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.h"

#include <optional>

namespace slm {

struct ManifestLoadResult
{
    bool ok = false;
    AppManifest manifest;
    QString error;
};

ManifestLoadResult loadAppManifest(const QString &path);
bool isSafeInstallDirName(const QString &name);

} // namespace slm
