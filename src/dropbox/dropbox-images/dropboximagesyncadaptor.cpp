/****************************************************************************
 **
 ** Copyright (C) 2015 Jolla Ltd.
 ** Contact: Jonni Rainisto <jonni.rainisto@jollamobile.com>
 **
 ** This program/library is free software; you can redistribute it and/or
 ** modify it under the terms of the GNU Lesser General Public License
 ** version 2.1 as published by the Free Software Foundation.
 **
 ** This program/library is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 ** Lesser General Public License for more details.
 **
 ** You should have received a copy of the GNU Lesser General Public
 ** License along with this program/library; if not, write to the Free
 ** Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 ** 02110-1301 USA
 **
 ****************************************************************************/

#include "dropboximagesyncadaptor.h"
#include "trace.h"

#include <QtCore/QPair>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QVariantMap>
#include <QtCore/QByteArray>
#include <QtCore/QUrlQuery>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>
#include <QtSql/QSqlRecord>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>

#include <MGConfItem>

// Update the following version if database schema changes e.g. new
// fields are added to the existing tables.
// It will make old tables dropped and creates new ones.

// Currently, we integrate with the device image gallery via saving thumbnails to the
// ~/.config/sociald/images directory, and filling the ~/.config/sociald/images/dropbox.db
// with appropriate data.

DropboxImageSyncAdaptor::DropboxImageSyncAdaptor(QObject *parent)
    : DropboxDataTypeSyncAdaptor(SocialNetworkSyncAdaptor::Images, parent)
    , m_optimalThumbnailWidth(0)
    , m_optimalImageWidth(0)
{
    setInitialActive(m_db.isValid());
}

DropboxImageSyncAdaptor::~DropboxImageSyncAdaptor()
{
}

QString DropboxImageSyncAdaptor::syncServiceName() const
{
    return QStringLiteral("dropbox-images");
}

void DropboxImageSyncAdaptor::sync(const QString &dataTypeString, int accountId)
{
    // get ready for sync
    if (!determineOptimalDimensions()) {
        SOCIALD_LOG_ERROR("unable to determine optimal image dimensions, aborting");
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }
    if (!initRemovalDetectionLists(accountId)) {
        SOCIALD_LOG_ERROR("unable to initialized cached account list for account" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    // call superclass impl.
    DropboxDataTypeSyncAdaptor::sync(dataTypeString, accountId);
}

void DropboxImageSyncAdaptor::purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode)
{
    m_db.purgeAccount(oldId);
    m_db.commit();
    m_db.wait();

    // manage image cache. Gallery UI caches full size images
    // and maintains bindings between source and cached image in SocialImageDatabase.
    // purge cached images belonging to this account.
    purgeCachedImages(&m_imageCacheDb, oldId);
}

void DropboxImageSyncAdaptor::beginSync(int accountId, const QString &accessToken)
{
    // Camera Roll is not return as an album, handle it as a special case
    queryCameraRoll(accountId, accessToken);
    requestAlbums(accountId, accessToken);
}

void DropboxImageSyncAdaptor::finalize(int accountId)
{
    Q_UNUSED(accountId)

    if (syncAborted()) {
        SOCIALD_LOG_INFO("sync aborted, won't commit database changes");
    } else {
        // Remove albums
        m_db.removeAlbums(m_cachedAlbums.keys());

        // Remove images
        m_db.removeImages(m_removedImages);

        m_db.commit();
        m_db.wait();

        // manage image cache. Gallery UI caches full size images
        // and maintains bindings between source and cached image in SocialImageDatabase.
        // purge cached images older than two weeks.
        purgeExpiredImages(&m_imageCacheDb, accountId);
    }
}

void DropboxImageSyncAdaptor::queryCameraRoll(int accountId, const QString &accessToken)
{
    QUrl url(QStringLiteral("%1/1/metadata/auto/Pictures").arg(api())); // TODO Pictures foldername from translated languages
    QNetworkRequest req;
    req.setUrl(url);
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ")).toUtf8() + accessToken.toUtf8());

    QNetworkReply *reply = m_networkAccessManager->get(req);
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(cameraRollFinishedHandler()));

        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply);
    } else {
        SOCIALD_LOG_ERROR("unable to request data from Dropbox account with id" << accountId);
        clearRemovalDetectionLists(); // don't perform server-side removal detection during this sync run.
    }
}

void DropboxImageSyncAdaptor::requestAlbums(int accountId, const QString &accessToken)
{
    QUrl url(QStringLiteral("%1/1/metadata/auto/").arg(api())); // TODO
    QNetworkRequest req;
    req.setUrl(url);
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ")).toUtf8() + accessToken.toUtf8());

    QNetworkReply *reply = m_networkAccessManager->get(req);
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(albumsFinishedHandler()));

        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply);
    } else {
        SOCIALD_LOG_ERROR("unable to request data from Dropbox account with id" << accountId);
        clearRemovalDetectionLists(); // don't perform server-side removal detection during this sync run.
    }
}

void DropboxImageSyncAdaptor::requestImages(int accountId, const QString &accessToken,
                                             const QString &albumId, const QString &userId,
                                             const QString &nextRound)
{
    QString path = nextRound.isEmpty() ? QStringLiteral("%1/files/?filter=photos&limit=100").arg(albumId) // TODO FIX to dropbox
                                       : nextRound;

    QUrl url(QStringLiteral("%1/1/files/auto/%2").arg(content()).arg(path)); //TODO
    QNetworkRequest req;
    req.setUrl(url);
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ")).toUtf8() + accessToken.toUtf8());

    QNetworkReply *reply = m_networkAccessManager->get(req);
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("albumId", albumId);
        reply->setProperty("userId", userId);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(imagesFinishedHandler()));

        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        if (nextRound.isEmpty()) {
            incrementSemaphore(accountId);
        }
        setupReplyTimeout(accountId, reply);
    } else {
        SOCIALD_LOG_ERROR("unable to request data from Dropbox account with id" << accountId);
        clearRemovalDetectionLists(); // don't perform server-side removal detection during this sync run.
    }
}

void DropboxImageSyncAdaptor::cameraRollFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    bool isError = reply->property("isError").toBool();
    int accountId = reply->property("accountId").toInt();
    QString accessToken = reply->property("accessToken").toString();
    QString continuationUrl = reply->property("continuationUrl").toString();
    QByteArray replyData = reply->readAll();
    disconnect(reply);
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);

    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(replyData, &ok);
    if (isError || !ok) {
        SOCIALD_LOG_ERROR("unable to read albums response for Dropbox account with id" << accountId);
        clearRemovalDetectionLists(); // don't perform server-side removal detection during this sync run.
        decrementSemaphore(accountId);
        return;
    }

    QString albumId = parsed.value(QLatin1String("id")).toString();
    QString albumName = parsed.value(QLatin1String("name")).toString();
    QString createdTimeStr = parsed.value(QLatin1String("created_time")).toString();
    QString updatedTimeStr = parsed.value(QLatin1String("updated_time")).toString();
    int imageCount = static_cast<int>(parsed.value(QLatin1String("count")).toDouble());

    QJsonObject from = parsed.value(QLatin1String("from")).toObject();
    if (from.isEmpty()) {
        SOCIALD_LOG_ERROR("camera roll doesn't contain user iformation for Dropbox account with id" << accountId);
        clearRemovalDetectionLists(); // don't perform server-side removal detection during this sync run.
        decrementSemaphore(accountId);
        return;
    }

    QString userId = from.value(QLatin1String("id")).toString();;
    QDateTime createdTime = QDateTime::fromString(createdTimeStr, Qt::ISODate);
    QDateTime updatedTime = QDateTime::fromString(updatedTimeStr, Qt::ISODate);

    const DropboxAlbum::ConstPtr &dbAlbum = m_cachedAlbums.value(albumId);
    m_cachedAlbums.remove(albumId);  // Removal detection
    if (!dbAlbum.isNull() && (dbAlbum->updatedTime() >= updatedTime
                              && dbAlbum->imageCount() == imageCount)) {
        SOCIALD_LOG_DEBUG("album with id" << albumId << "by user" << userId <<
                          "from Dropbox account with id" << accountId << "doesn't need sync");
        decrementSemaphore(accountId);
        return;
    }

    possiblyAddNewUser(userId, accountId, accessToken);
    m_db.syncAccount(accountId, userId);
    m_db.addAlbum(albumId, userId, createdTime, updatedTime, albumName, imageCount);

    requestImages(accountId, accessToken, albumId, userId);

    // Finally, reduce our semaphore.
    decrementSemaphore(accountId);
}

void DropboxImageSyncAdaptor::albumsFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    bool isError = reply->property("isError").toBool();
    int accountId = reply->property("accountId").toInt();
    QString accessToken = reply->property("accessToken").toString();
    QString continuationUrl = reply->property("continuationUrl").toString();
    QByteArray replyData = reply->readAll();
    disconnect(reply);
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);

    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(replyData, &ok);
    if (isError || !ok || !parsed.contains(QLatin1String("data"))) {
        SOCIALD_LOG_ERROR("unable to read albums response for Dropbox account with id" << accountId);
        clearRemovalDetectionLists(); // don't perform server-side removal detection during this sync run.
        decrementSemaphore(accountId);
        return;
    }

    QJsonArray data = parsed.value(QLatin1String("data")).toArray();
    if (data.size() == 0) {
        SOCIALD_LOG_DEBUG("Dropbox account with id" << accountId << "has no albums");
        decrementSemaphore(accountId);
        return;
    }

    // read the albums information
    for (int i = 0; i < data.size(); ++i) {
        QJsonObject albumObject = data.at(i).toObject();
        if (albumObject.isEmpty()) {
            continue;
        }

        QString albumId = albumObject.value(QLatin1String("id")).toString();
        QString userId = albumObject.value(QLatin1String("from")).toObject().value(QLatin1String("id")).toString();
        if (!userId.isEmpty()) {
            m_db.syncAccount(accountId, userId);
        }

        QString albumName = albumObject.value(QLatin1String("name")).toString();
        QString createdTimeStr = albumObject.value(QLatin1String("created_time")).toString();
        QString updatedTimeStr = albumObject.value(QLatin1String("updated_time")).toString();
        int imageCount = static_cast<int>(albumObject.value(QLatin1String("count")).toDouble());

        // check to see whether we need to sync (any changes since last sync)
        // Note that we also check if the image count is the same, since, when
        // removing an image, the updatedTime is not changed
        QDateTime createdTime = QDateTime::fromString(createdTimeStr, Qt::ISODate);
        QDateTime updatedTime = QDateTime::fromString(updatedTimeStr, Qt::ISODate);

        const DropboxAlbum::ConstPtr &dbAlbum = m_cachedAlbums.value(albumId);
        m_cachedAlbums.remove(albumId);  // Removal detection
        if (!dbAlbum.isNull() && (dbAlbum->updatedTime() >= updatedTime
                                  && dbAlbum->imageCount() == imageCount)) {
            SOCIALD_LOG_DEBUG("album with id" << albumId << "by user" << userId <<
                              "from Dropbox account with id" << accountId << "doesn't need sync");
            continue;
        }

        // We need to sync. We save the album entry, and request the images for the album.
        // When saving the album, we might need to add a new user
        possiblyAddNewUser(userId, accountId, accessToken);

        // We then save the album
        m_db.addAlbum(albumId, userId, createdTime, updatedTime, albumName, imageCount);
        requestImages(accountId, accessToken, albumId, userId);
    }

    // Finally, reduce our semaphore.
    decrementSemaphore(accountId);
}

void DropboxImageSyncAdaptor::imagesFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    bool isError = reply->property("isError").toBool();
    int accountId = reply->property("accountId").toInt();
    QString accessToken = reply->property("accessToken").toString();
    QString userId = reply->property("userId").toString();
    QString albumId = reply->property("albumId").toString();
    QByteArray replyData = reply->readAll();
    disconnect(reply);
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);

    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(replyData, &ok);
    if (isError || !ok || !parsed.contains(QLatin1String("data"))) {
        SOCIALD_LOG_ERROR("unable to read photos response for Dropbox account with id" << accountId);
        clearRemovalDetectionLists(); // don't perform server-side removal detection during this sync run.
        decrementSemaphore(accountId);
        return;
    }

    QJsonArray data = parsed.value(QLatin1String("data")).toArray();
    if (data.size() == 0) {
        SOCIALD_LOG_DEBUG("album with id" << albumId << "from Dropbox account with id" << accountId << "has no photos");
        checkRemovedImages(albumId);
        decrementSemaphore(accountId);
        return;
    }

    // read the photos information
    foreach (const QJsonValue imageValue, data) {
        QJsonObject imageObject = imageValue.toObject();
        if (imageObject.isEmpty()) {
            continue;
        }

        QString type = imageObject.value(QLatin1String("type")).toString();
        if (type != QStringLiteral("photo")) {
            // should not happen as we have a filter in request, but just to be sure
            continue;
        }

        QString photoId = imageObject.value(QLatin1String("id")).toString();
        QString thumbnailUrl = imageObject.value(QLatin1String("picture")).toString();
        QString imageSrcUrl = imageObject.value(QLatin1String("location")).toString();
        QString createdTimeStr = imageObject.value(QLatin1String("created_time")).toString();
        QString updatedTimeStr = imageObject.value(QLatin1String("updated_time")).toString();
        QString photoName = imageObject.value(QLatin1String("name")).toString();
        int imageWidth = 0;
        int imageHeight = 0;

        // Find optimal thumbnail and image source urls based on dimensions.
        QList<ImageSource> imageSources;
        QJsonArray images = imageObject.value(QLatin1String("images")).toArray();
        foreach (const QJsonValue &imageValue, images) {
            QJsonObject image = imageValue.toObject();
            imageSources << ImageSource(static_cast<int>(image.value(QLatin1String("width")).toDouble()),
                                        static_cast<int>(image.value(QLatin1String("height")).toDouble()),
                                        image.value(QLatin1String("source")).toString());
        }

        bool foundOptimalImage = false, foundOptimalThumbnail = false;
        std::sort(imageSources.begin(), imageSources.end());
        Q_FOREACH (const ImageSource &img, imageSources) {
            if (!foundOptimalThumbnail && qMin(img.width, img.height) >= m_optimalThumbnailWidth) {
                foundOptimalThumbnail = true;
                thumbnailUrl = img.sourceUrl;
            }
            if (!foundOptimalImage && qMin(img.width, img.height) >= m_optimalImageWidth) {
                foundOptimalImage = true;
                imageWidth = img.width;
                imageHeight = img.height;
                imageSrcUrl = img.sourceUrl;
            }
        }
        if (!foundOptimalThumbnail) {
            // just choose the largest one.
            thumbnailUrl = imageSources.last().sourceUrl;
        }
        if (!foundOptimalImage) {
            // just choose the largest one.
            imageSrcUrl = imageSources.last().sourceUrl;
            imageWidth = imageSources.last().width;
            imageHeight = imageSources.last().height;
        }

        QDateTime createdTime = QDateTime::fromString(createdTimeStr, Qt::ISODate);
        QDateTime updatedTime = QDateTime::fromString(updatedTimeStr, Qt::ISODate);
        if (!m_serverImageIds[albumId].contains(photoId)) {
            m_serverImageIds[albumId].insert(photoId);
        }

        // check if we need to sync, and write to the database.
        if (haveAlreadyCachedImage(photoId, imageSrcUrl)) {
            SOCIALD_LOG_DEBUG("have previously cached photo" << photoId << ":" << imageSrcUrl);
        } else {
            SOCIALD_LOG_DEBUG("caching new photo" << photoId << ":" << imageSrcUrl << "->" << imageWidth << "x" << imageHeight);
            m_db.addImage(photoId, albumId, userId, createdTime, updatedTime,
                          photoName, imageWidth, imageHeight, thumbnailUrl, imageSrcUrl);
        }
    }
    // perform a continuation request if required.
    QJsonObject paging = parsed.value(QLatin1String("paging")).toObject();
    if (!paging.isEmpty()) {
        QString next = paging.value(QLatin1String("next")).toString();
        if (!next.isEmpty()) {
            SOCIALD_LOG_DEBUG("performing continuation request for more photos for Dropbox account with id" << accountId << ":" << next);
            requestImages(accountId, accessToken, albumId, userId, next);
            return;
        }
    }

    checkRemovedImages(albumId);
    // we're finished this request.  Decrement our busy semaphore.
    decrementSemaphore(accountId);
}

bool DropboxImageSyncAdaptor::haveAlreadyCachedImage(const QString &imageId, const QString &imageUrl)
{
    DropboxImage::ConstPtr dbImage = m_db.image(imageId);
    bool imagedbSynced = !dbImage.isNull();

    if (!imagedbSynced) {
        return false;
    }

    QString dbImageUrl = dbImage->imageUrl();
    if (dbImageUrl != imageUrl) {
        SOCIALD_LOG_ERROR("Image/dropbox.db has outdated data!\n"
                          "   photoId:" << imageId << "\n"
                          "   cached image url:" << dbImageUrl << "\n"
                          "   new image url:" << imageUrl);
        return false;
    }

    return true;
}

void DropboxImageSyncAdaptor::possiblyAddNewUser(const QString &userId, int accountId,
                                                  const QString &accessToken)
{
    if (!m_db.user(userId).isNull()) {
        return;
    }

    // We need to add the user. We call Dropbox to get the informations that we
    // need and then add it to the database
    // me?fields=updated_time,name,picture
    QUrl url(QStringLiteral("%1/%2?access_token=%3").arg(api()).arg(userId).arg(accessToken)); // TODO
    QList<QPair<QString, QString> > queryItems;
    queryItems.append(QPair<QString, QString>(QString(QLatin1String("fields")),
                                              QLatin1String("id,updated_time,name,picture")));
    QUrlQuery query(url);
    query.setQueryItems(queryItems);
    url.setQuery(query);
    QNetworkRequest req;
    req.setUrl(url);
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ")).toUtf8() + accessToken.toUtf8());

    QNetworkReply *reply = m_networkAccessManager->get(req);
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)),
                this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)),
                this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(userFinishedHandler()));

        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply);
    }
}

void DropboxImageSyncAdaptor::userFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    QByteArray replyData = reply->readAll();
    int accountId = reply->property("accountId").toInt();
    disconnect(reply);
    reply->deleteLater();

    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(replyData, &ok);
    if (!ok || !parsed.contains(QLatin1String("id"))) {
        SOCIALD_LOG_ERROR("unable to read user response for Dropbox account with id" << accountId);
        return;
    }

    QString userId = parsed.value(QLatin1String("id")).toString();
    QString name = parsed.value(QLatin1String("name")).toString();

    m_db.addUser(userId, QDateTime::currentDateTime(), name);
    decrementSemaphore(accountId);
}

bool DropboxImageSyncAdaptor::initRemovalDetectionLists(int accountId)
{
    // This function should be called as part of the ::sync() preamble.
    // Clear our internal state variables which we use to track server-side deletions.
    // We have to do it this way, as results can be spread across multiple requests
    // if Dropbox returns results in paginated form.
    clearRemovalDetectionLists();

    bool ok = false;
    QMap<int,QString> accounts = m_db.accounts(&ok);
    if (!ok) {
        return false;
    }
    if (accounts.contains(accountId)) {
        QString userId = accounts.value(accountId);

        QStringList allAlbumIds = m_db.allAlbumIds();
        foreach (const QString& albumId, allAlbumIds) {
            DropboxAlbum::ConstPtr album = m_db.album(albumId);
            if (album->userId() == userId) {
                m_cachedAlbums.insert(albumId, album);
            }
        }
    }

    return true;
}

void DropboxImageSyncAdaptor::clearRemovalDetectionLists()
{
    m_cachedAlbums.clear();
    m_serverImageIds.clear();
    m_removedImages.clear();
}

void DropboxImageSyncAdaptor::checkRemovedImages(const QString &albumId)
{
    const QSet<QString> &serverImageIds = m_serverImageIds.value(albumId);
    QSet<QString> cachedImageIds = m_db.imageIds(albumId).toSet();

    foreach (const QString &imageId, serverImageIds) {
        cachedImageIds.remove(imageId);
    }

    m_removedImages.append(cachedImageIds.toList());
}

bool DropboxImageSyncAdaptor::determineOptimalDimensions()
{
    int width = 0, height = 0;
    const int defaultValue = 0;
    MGConfItem widthConf("/lipstick/screen/primary/width");
    if (widthConf.value(defaultValue).toInt() != defaultValue) {
        width = widthConf.value(defaultValue).toInt();
    }
    MGConfItem heightConf("/lipstick/screen/primary/height");
    if (heightConf.value(defaultValue).toInt() != defaultValue) {
        height = heightConf.value(defaultValue).toInt();
    }

    // we want to use the largest of these dimensions as the "optimal"
    int maxDimension = qMax(width, height);
    if (maxDimension % 3 == 0) {
        m_optimalThumbnailWidth = maxDimension / 3;
    } else {
        m_optimalThumbnailWidth = (maxDimension / 2);
    }
    m_optimalImageWidth = maxDimension;
    SOCIALD_LOG_DEBUG("Determined optimal image dimension:" << m_optimalImageWidth << ", thumbnail:" << m_optimalThumbnailWidth);
    return true;
}
