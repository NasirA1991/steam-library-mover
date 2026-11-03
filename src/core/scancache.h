// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.h"

#include <QString>

namespace slm::ScanCache {

// A non-authoritative, per-root snapshot used only to paint the window while
// the real scan runs. Moves are never planned from this data.
bool load(const SteamRoot &root, SteamScan *scan, QString *error = nullptr);
bool save(const SteamScan &scan, QString *error = nullptr);
QString cachePath(const SteamRoot &root);

} // namespace slm::ScanCache
