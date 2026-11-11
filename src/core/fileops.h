// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace slm::fsutil {

bool writeFileAtomic(const QString &path, const QByteArray &data, int mode, QString *error = nullptr);
bool fsyncDir(const QString &dir, QString *error = nullptr);
bool syncFilesystem(const QString &path, QString *error = nullptr);
bool ensureDir(const QString &path, QString *error = nullptr);
// Recursively removes path without following symlinks. Traversal is anchored at
// directory descriptors and refuses to cross a filesystem boundary.
bool removeTree(const QString &path, QString *error = nullptr);
bool isSameFilesystem(const QString &a, const QString &b);
QString uniqueSuffix();
std::optional<QByteArray> readFile(const QString &path, qint64 maxSize, QString *error = nullptr);
std::optional<QByteArray> readSymlinkTarget(const QString &path, QString *error = nullptr);

} // namespace slm::fsutil
