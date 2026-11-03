// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace ui {

enum class Theme { System, Light, Dark };

Theme themeFromSettings();
Theme themeFromName(const QString &name);
QString themeName(Theme theme);
void applyTheme(Theme theme);
bool isDarkTheme();

} // namespace ui
