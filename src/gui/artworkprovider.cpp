// SPDX-License-Identifier: GPL-3.0-or-later
#include "artworkprovider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

namespace {

constexpr int kMaxConcurrent = 4;

QString defaultCacheDir()
{
    const QString override = qEnvironmentVariable("SLM_ARTWORK_DIR");
    if (!override.isEmpty())
        return QDir::cleanPath(override);
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.cache");
    return base + QStringLiteral("/steam-library-mover/artwork");
}

QString artworkUrl(quint32 appId, int attempt)
{
    if (attempt == 0) {
        return QStringLiteral("https://cdn.cloudflare.steamstatic.com/steam/apps/%1/"
                              "library_600x900.jpg")
            .arg(appId);
    }
    return QStringLiteral("https://cdn.cloudflare.steamstatic.com/steam/apps/%1/header.jpg")
        .arg(appId);
}

} // namespace

ArtworkProvider::ArtworkProvider(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_cacheDir(defaultCacheDir())
    , m_enabled(!qEnvironmentVariableIsSet("SLM_DISABLE_ARTWORK"))
{
    QDir().mkpath(m_cacheDir);
}

void ArtworkProvider::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    if (!enabled) {
        m_queue.clear();
        m_inFlight.clear();
        m_failed.clear();
    } else {
        // Allow previously failed downloads to be retried after the setting is
        // re-enabled or the user comes back online.
        m_failed.clear();
    }
}

void ArtworkProvider::setCacheDir(const QString &path)
{
    if (path.isEmpty())
        return;
    m_cacheDir = QDir::cleanPath(path);
    QDir().mkpath(m_cacheDir);
}

QString ArtworkProvider::cachePath(quint32 appId) const
{
    return m_cacheDir + QStringLiteral("/%1_library_600x900.jpg").arg(appId);
}

void ArtworkProvider::request(quint32 appId, const QString &localArtwork)
{
    if (appId == 0 || !m_enabled)
        return;
    if (!localArtwork.isEmpty() && QFileInfo::exists(localArtwork)
        && QFileInfo(localArtwork).isFile()) {
        emit artworkReady(appId, localArtwork);
        return;
    }
    const QString cached = cachePath(appId);
    if (QFileInfo::exists(cached) && QFileInfo(cached).isFile()) {
        emit artworkReady(appId, cached);
        return;
    }
    if (m_inFlight.contains(appId) || m_failed.contains(appId))
        return;

    m_inFlight.insert(appId);
    m_queue.enqueue({appId, 0});
    processQueue();
}

void ArtworkProvider::processQueue()
{
    while (m_active < kMaxConcurrent && !m_queue.isEmpty()) {
        const Pending pending = m_queue.dequeue();
        if (!m_enabled || m_failed.contains(pending.appId) || pending.attempt > 1) {
            m_inFlight.remove(pending.appId);
            continue;
        }
        fetch(pending);
    }
}

void ArtworkProvider::fetch(const Pending &pending)
{
    QNetworkRequest request(QUrl(artworkUrl(pending.appId, pending.attempt)));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("SteamLibraryMover/0.1.0"));
    request.setRawHeader("Accept", "image/jpeg,image/png");
    request.setTransferTimeout(10000);

    QNetworkReply *reply = m_network->get(request);
    m_replies.insert(reply, pending);
    ++m_active;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { finishReply(reply); });
}

void ArtworkProvider::finishReply(QNetworkReply *reply)
{
    const Pending pending = m_replies.take(reply);
    --m_active;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray data = reply->readAll();
    const bool ok = reply->error() == QNetworkReply::NoError && status == 200 && !data.isEmpty();
    reply->deleteLater();

    if (!m_enabled) {
        m_inFlight.remove(pending.appId);
    } else if (ok) {
        if (!QDir().mkpath(m_cacheDir)) {
            m_failed.insert(pending.appId);
            m_inFlight.remove(pending.appId);
            processQueue();
            return;
        }
        const QString path = cachePath(pending.appId);
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit()) {
            m_inFlight.remove(pending.appId);
            emit artworkReady(pending.appId, path);
        } else {
            m_failed.insert(pending.appId);
            m_inFlight.remove(pending.appId);
        }
    } else if (pending.attempt == 0) {
        // The portrait capsule may not exist for every app; fall back to the header image.
        m_queue.prepend({pending.appId, 1});
    } else {
        m_failed.insert(pending.appId);
        m_inFlight.remove(pending.appId);
    }
    processQueue();
}
