// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace ui {
bool sendDesktopNotification(const QString &title, const QString &body, QString *error = nullptr);
}
