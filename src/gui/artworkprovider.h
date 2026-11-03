// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

class ArtworkProvider : public QObject
{
    Q_OBJECT
public:
    explicit ArtworkProvider(QObject *parent = nullptr);

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    void setCacheDir(const QString &path);
    QString cacheDir() const { return m_cacheDir; }

    void request(quint32 appId, const QString &localArtwork = QString());

signals:
    void artworkReady(quint32 appId, const QString &path);

private:
    struct Pending
    {
        quint32 appId = 0;
        int attempt = 0;
    };

    void processQueue();
    void fetch(const Pending &pending);
    void finishReply(QNetworkReply *reply);
    QString cachePath(quint32 appId) const;

    QNetworkAccessManager *m_network = nullptr;
    QQueue<Pending> m_queue;
    QSet<quint32> m_inFlight;
    QSet<quint32> m_failed;
    QHash<QNetworkReply *, Pending> m_replies;
    QString m_cacheDir;
    bool m_enabled = true;
    int m_active = 0;
};
