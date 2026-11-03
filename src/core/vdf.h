// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <optional>

namespace slm {

struct VdfNode
{
    QString key;
    QString value;
    bool isObject = false;
    QVector<VdfNode> children;

    const VdfNode *child(const QString &childKey) const;
    VdfNode *child(const QString &childKey);
    const VdfNode *object(const QString &childKey) const;
    VdfNode *object(const QString &childKey);
    QString childValue(const QString &childKey) const;
    bool setChildValue(const QString &childKey, const QString &value);
    bool removeChild(const QString &childKey);
};

class VdfDocument
{
public:
    VdfNode root;

    static std::optional<VdfDocument> parse(const QByteArray &data, QString *error = nullptr,
                                            int *errorOffset = nullptr);
    QByteArray serialize() const;

    const VdfNode *object(const QString &key) const;
    VdfNode *object(const QString &key);

private:
    static bool parseInternal(const QByteArray &data, VdfDocument &out, QString *error,
                              int *errorOffset);
};

QString vdfEscape(const QString &value);
QString vdfUnescape(const QByteArray &value);

} // namespace slm
