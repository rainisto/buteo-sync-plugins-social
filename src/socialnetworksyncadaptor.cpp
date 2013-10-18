/****************************************************************************
 **
 ** Copyright (C) 2013 Jolla Ltd.
 ** Contact: Chris Adams <chris.adams@jollamobile.com>
 **
 ****************************************************************************/

#include "socialnetworksyncadaptor.h"
#include "syncservice.h"
#include "trace.h"

#include <QtCore/QJsonDocument>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>
#include <QtSql/QSqlRecord>

#include <QtNetwork/QNetworkAccessManager>

// sailfish-components-accounts-qt5
#include <accountmanager.h>
#include <account.h>

#include <socialnetworksyncdatabase.h>

/*
    Remarks on timestamps

    The timezone issue is a pretty big one, as services might
    provide time in different timezone, and that the user might
    be in another timezone.

    To make everything consistant, all time should be stored using
    UTC time. It is because Qt might have some troubles storing
    timezone in SQLITE databases. (Or is it SQLITE that have
    some troubles with timezone ?)

    Beware however, that all our APIs (eventfeed, notification)
    uses local time. So you have to perform a conversion before
    using date and time retrieved from the SQLITE database.

    By convention, all methods in sociald returning QDateTime
    object will return the UTC time, with their timeSpec set to
    Qt::UTC. Be sure to perform conversion before using them.
*/

SocialNetworkSyncAdaptor::SocialNetworkSyncAdaptor(QString serviceName,
                                                   SyncService::DataType dataType,
                                                   SyncService *syncService, QObject *parent)
    : QObject(parent)
    , dataType(dataType)
    , accountManager(new AccountManager(this))
    , networkAccessManager(new QNetworkAccessManager(this))
    , m_syncDb(new SocialNetworkSyncDatabase())
    , m_status(SocialNetworkSyncAdaptor::Invalid)
    , m_serviceName(serviceName)
    , m_syncService(syncService)
{
    qWarning() << "Initializing database";
    m_syncDb->initDatabase();
}

SocialNetworkSyncAdaptor::~SocialNetworkSyncAdaptor()
{
    delete m_syncDb;
}

SocialNetworkSyncAdaptor::Status SocialNetworkSyncAdaptor::status() const
{
    return m_status;
}

bool SocialNetworkSyncAdaptor::enabled() const
{
    return m_enabled;
}

QString SocialNetworkSyncAdaptor::serviceName() const
{
    return m_serviceName;
}

void SocialNetworkSyncAdaptor::sync(const QString &dataType)
{
    Q_UNUSED(dataType)
    TRACE(SOCIALD_ERROR, QString(QLatin1String("error: should be overridden by derived types")));
}

void SocialNetworkSyncAdaptor::checkAccounts(SyncService::DataType dataType, QList<int> *newIds, QList<int> *purgeIds, QList<int> *updateIds)
{
    QList<int> knownIds = syncedAccounts(SyncService::dataType(dataType));
    QList<int> currentIds = accountManager->accountIdentifiers();
    TRACE(SOCIALD_DEBUG,
            QString(QLatin1String("have found %1 accounts which support a sync service; determining old/new/update sets..."))
            .arg(currentIds.size()));

    foreach (int currId, currentIds) {
        Account *act = accountManager->account(currId);
        if (!act || !(act->supportedServiceNames().size() > 0 &&
                      act->supportedServiceNames().at(0).startsWith(m_serviceName))) {
            TRACE(SOCIALD_DEBUG,
                    QString(QLatin1String("account %1 does not support service %2, ignoring"))
                    .arg(currId).arg(m_serviceName));
            continue; // not same account as m_serviceName.  Ignore it.
        }

        // if the account has been disabled with the sync service, we purge it.
        if (act->isEnabledWithService(QString(QLatin1String("%1-sync")).arg(m_serviceName))) {
            if (knownIds.contains(currId)) {
                knownIds.removeAll(currId);
                updateIds->append(currId);
            } else {
                newIds->append(currId);
            }
        }
    }

    // anything left in knownIds must belong to an old, removed account.
    foreach (int id, knownIds) {
        purgeIds->append(id);
    }
}

/*!
    \internal
    Called when the semaphores decreased to 0, this method is used
    to finalize something, like saving all data to a database.
    
    You can call incrementSemaphore to perform asynchronous tasks
    in this method. finalize will then be called again when the 
    asynchronous task is finished (and when decrementSemaphore is
    called), be sure to have a condition check in order not to run
    into an infinite loop.
    
    It is unsafe to call decrementSemaphore in this method, as 
    the semaphore handling method will find that the semaphore
    went to 0 twice and will perform cleanup operations twice.
    Please call decrementSemaphore at the end of the asynchronous
    task (preferably in a slot), and only call incrementSemaphore 
    for asynchronous tasks.
 */
void SocialNetworkSyncAdaptor::finalize(int accountId)
{
    Q_UNUSED(accountId)
}

/*!
    \internal
    Returns the last sync timestamp for the given service, account and data type.
    If data from prior to this timestamp is received in subsequent requests, it does not need to be synced.
    This function will return an invalid QDateTime if no synchronisation has occurred.
*/
QDateTime SocialNetworkSyncAdaptor::lastSyncTimestamp(const QString &serviceName,
                                                      const QString &dataType,
                                                      int accountId) const
{
    return m_syncDb->lastSyncTimestamp(serviceName, dataType, accountId);
}

/*!
    \internal
    Updates the last sync timestamp for the given service, account and data type to the given \a timestamp.
*/
bool SocialNetworkSyncAdaptor::updateLastSyncTimestamp(const QString &serviceName,
                                                       const QString &dataType,
                                                       int accountId,
                                                       const QDateTime &timestamp)
{
    // Workaround
    // TODO: do better, with a queue
    m_syncDb->addSyncTimestamp(serviceName, dataType, accountId, timestamp);
    return m_syncDb->write();
}

/*!
    \internal
    Returns the synced timestamp for datum identified by the given \a datumIdentifier.
    If the given datum hasn't been marked as synced, this function will return an invalid QDateTime.
*/
QDateTime SocialNetworkSyncAdaptor::whenSyncedDatum(const QString &serviceName, const QString &datumIdentifier) const
{
    if (!m_syncService->database()) {
        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: database not available")));
        return QDateTime();
    }

    QSqlQuery query(*m_syncService->database());
    query.prepare("SELECT syncTimestamp FROM syncedData WHERE serviceName = :serviceName AND datumIdentifier = :datumIdentifier");
    query.bindValue(":serviceName", serviceName);
    query.bindValue(":datumIdentifier", datumIdentifier);
    bool success = query.exec();
    if (!success) {
        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: unable to execute query: %1")).arg(query.lastError().text()));
        return QDateTime();
    }

    if (query.next()) {
        QDateTime dateTime = QDateTime::fromString(query.value(0).toString(), Qt::ISODate);
        dateTime.setTimeSpec(Qt::UTC);
        return dateTime;
    }

    return QDateTime();
}

bool SocialNetworkSyncAdaptor::markSyncedData(const QList<SyncedDatum> &data)
{
    if (!m_syncService->database()) {
        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: database not available")));
        return false;
    }

    QSqlQuery query (*(m_syncService->database()));

    // Use this query to speedup SQLITE insertion
    // Create a transaction
    if (!query.exec("BEGIN EXCLUSIVE TRANSACTION")) {
        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: unable to execute query: %1")).arg(query.lastError().text()));
        return false;
    }

    // Prepare first batch query
    QVariantList localIdentifierList;
    QVariantList serviceNameList;
    QVariantList dataTypeList;
    QVariantList createdTimestampList;
    QVariantList syncTimestampList;
    QVariantList datumIdentifierList;
    QVariantList accountIdentifierList;

    foreach (const SyncedDatum &datum, data) {
        QDateTime trueCreatedTimestamp = datum.createdTimestamp;
        if (trueCreatedTimestamp.timeSpec() == Qt::LocalTime) {
            trueCreatedTimestamp = datum.createdTimestamp.toUTC();
        }

        QDateTime trueSyncedTimestamp = datum.syncedTimestamp;
        if (trueSyncedTimestamp.timeSpec() == Qt::LocalTime) {
            trueSyncedTimestamp = datum.syncedTimestamp.toUTC();
        }


        localIdentifierList.append(datum.localIdentifier);
        serviceNameList.append(datum.serviceName);
        dataTypeList.append(datum.dataType);
        createdTimestampList.append(trueCreatedTimestamp);
        syncTimestampList.append(trueSyncedTimestamp);
        datumIdentifierList.append(datum.datumIdentifier);
        accountIdentifierList.append(datum.accountIdentifier);
    }

    // We insert, or update a field that already exist
    query.prepare("INSERT OR REPLACE INTO syncedData (localIdentifier, serviceName, dataType, createdTimestamp, syncTimestamp, datumIdentifier) VALUES (?, ?, ?, ?, ?, ?)");
    query.addBindValue(localIdentifierList);
    query.addBindValue(serviceNameList);
    query.addBindValue(dataTypeList);
    query.addBindValue(createdTimestampList);
    query.addBindValue(syncTimestampList);
    query.addBindValue(datumIdentifierList);

    if (!query.execBatch()) {
        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: unable to execute query: %1")).arg(query.lastError().text()));
        return false;
    }

    query.prepare("INSERT OR REPLACE INTO link_syncedData_account (syncedDataId, accountIdentifier) VALUES (?, ?)");
    query.addBindValue(localIdentifierList);
    query.addBindValue(accountIdentifierList);

    if (!query.execBatch()) {
        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: unable to execute query: %1")).arg(query.lastError().text()));
        return false;
    }

    // Execute transaction
    if (!query.exec("END TRANSACTION")) {
        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: unable to execute query: %1")).arg(query.lastError().text()));
        return false;
    }

    return true;

}

QString SocialNetworkSyncAdaptor::syncedDatumLocalIdentifier(const QString &serviceName,
                                                             const QString &dataType,
                                                             const QString &datumIdentifier)
{
    if (!m_syncService->database()) {
        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: database not available")));
        return QString();
    }

    QSqlQuery query(*(m_syncService->database()));
    query.prepare("SELECT localIdentifier FROM syncedData WHERE serviceName = :serviceName AND dataType = :dataType AND datumIdentifier = :datumIdentifier");
    query.bindValue(":serviceName", serviceName);
    query.bindValue(":dataType", dataType);
    query.bindValue(":datumIdentifier", datumIdentifier);
    if (!query.exec()) {
        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: unable to execute query: %1")).arg(query.lastError().text()));
        return QString();
    }

    if (!query.next()) {
        return QString();
    }
    return query.value(0).toString();
}


/*!
    \internal
    Removes all rows from all sociald database tables which have the specified
    \a serviceName, \a dataType and \a accountId column values.
*/
QStringList SocialNetworkSyncAdaptor::removeAllData(const QString &serviceName,
                                                    const QString &dataType,
                                                    const QString &accountId, bool *ok)
{
    if (!m_syncService->database()) {
        if (ok) {
            *ok = false;
        }

        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: database not available")));
        return QStringList();
    }

    QSqlQuery query(*m_syncService->database());

    // Create a transaction
    if (!query.exec("BEGIN EXCLUSIVE TRANSACTION")) {
        if (ok) {
            *ok = false;
        }

        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: unable to execute query: %1")).arg(query.lastError().text()));
        return QStringList();
    }

    // First, we delete the links between the account to delete
    query.prepare("DELETE FROM link_syncedData_account WHERE syncedDataId "\
                  "IN (SELECT localIdentifier FROM syncedData "\
                  "WHERE serviceName=:serviceName AND dataType=:dataType) "\
                  "AND accountIdentifier=:accountId");
    query.bindValue(":serviceName", serviceName);
    query.bindValue(":dataType", dataType);
    query.bindValue(":accountId", accountId);
    if (!query.exec()) {
        if (ok) {
            *ok = false;
        }

        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: unable to execute query: %1")).arg(query.lastError().text()));
        return QStringList();
    }

    // We then select the localIdentifier that are not valid anymore
    // Basically, we select localIdentifier entries which do
    query.prepare("SELECT localIdentifier FROM synceddata "\
                  "LEFT JOIN  link_synceddata_account "\
                  "ON synceddata.localIdentifier = link_synceddata_account.syncedDataId "\
                  "WHERE accountIdentifier IS NULL GROUP BY localIdentifier");
    if (!query.exec()) {
        if (ok) {
            *ok = false;
        }

        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: unable to execute query: %1")).arg(query.lastError().text()));
        return QStringList();
    }

    QStringList localIdentifiers;
    QVariantList localIdentifiersVariant;
    while (query.next()) {
        localIdentifiers.append(query.value(0).toString());
        localIdentifiersVariant.append(query.value(0));
    }

    // Delete the old entries
    query.prepare("DELETE FROM syncedData WHERE localIdentifier=:localIdentifier");
    query.addBindValue(localIdentifiersVariant);
    if (!query.execBatch()) {
        if (ok) {
            *ok = false;
        }

        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: unable to execute query: %1")).arg(query.lastError().text()));
        return QStringList();
    }

    // Execute transaction
    if (!query.exec("END TRANSACTION")) {
        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: unable to execute query: %1")).arg(query.lastError().text()));
        if (ok) {
            *ok = false;
        }
        return QStringList();
    }

    if (ok) {
        *ok = true;
    }
    return localIdentifiers;
}

/*!
    \internal
    Returns the list of identifiers of accounts which have been synced for
    the given \a dataType.
*/
QList<int> SocialNetworkSyncAdaptor::syncedAccounts(const QString &dataType)
{
    return m_syncDb->syncedAccounts(m_serviceName, dataType);
}

QList<int> SocialNetworkSyncAdaptor::syncedDatumAccountIds(const QString &localIdentifier)
{
    if (!m_syncService->database()) {
        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: database not available")));
        return QList<int>();
    }

    QList<int> returnedData;

    QSqlQuery query(*m_syncService->database());
    query.prepare("SELECT accountIdentifier FROM link_syncedData_account WHERE syncedDataId=:localIdentifier");
    query.bindValue(":localIdentifier", localIdentifier);
    if (!query.exec()) {
        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: unable to execute query: %1")).arg(query.lastError().text()));
    }

    while (query.next()) {
        int accountIdentifier = query.value(0).toInt();
        returnedData.append(accountIdentifier);
    }

    return returnedData;
}

/*!
    \internal
    Begins a transaction.
    It is not necessary to invoke this function, but if you do you must call endTransaction()
    to commit any modifications made to the database.
*/
bool SocialNetworkSyncAdaptor::beginTransaction()
{
    if (!m_syncService->database()) {
        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: database not available")));
        return false;
    }

    m_syncService->database()->transaction();
    return true;
}

/*!
    \internal
    Commits all pending updates to the database.
    It is not necessary to invoke this function unless you previously called beginTransaction().
*/
bool SocialNetworkSyncAdaptor::endTransaction()
{
    if (!m_syncService->database()) {
        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: database not available")));
        return false;
    }

    m_syncService->database()->commit();
    return true;
}

/*!
 * \internal
 * Changes status if there is real change and emits statusChanged() signal.
 */
void SocialNetworkSyncAdaptor::setStatus(Status status)
{
    if (m_status != status) {
        m_status = status;
        emit statusChanged();
    }
}

/*!
 * \internal
 * Should be used in constructors to set the initial state
 * of enabled and status, without emitting signals
 *
 */
void SocialNetworkSyncAdaptor::setInitialActive(bool enabled)
{
    m_enabled = enabled;
    if (enabled) {
        m_status = Inactive;
    } else {
        m_status = Invalid;
    }
}

/*!
 * \internal
 * Should be called by any specific sync adapter when
 * they've finished syncing data.  The transition from
 * busy status to inactive status is what causes the
 * Buteo plugin to emit the sync results (and allows
 * subsequent syncs to occur).
 */
void SocialNetworkSyncAdaptor::setFinishedInactive()
{
    TRACE(SOCIALD_INFORMATION, QString(QLatin1String("Finished %1 %2 sync at: %3"))
                               .arg(m_serviceName, SyncService::dataType(dataType),
                                    QDateTime::currentDateTime().toString(Qt::ISODate)));
    setStatus(SocialNetworkSyncAdaptor::Inactive);
}

void SocialNetworkSyncAdaptor::incrementSemaphore(int accountId)
{
    int semaphoreValue = m_accountSyncSemaphores.value(accountId);
    semaphoreValue += 1;
    m_accountSyncSemaphores.insert(accountId, semaphoreValue);
    TRACE(SOCIALD_DEBUG, QString(QLatin1String("incremented busy semaphore for account %1 to %2")).arg(accountId).arg(semaphoreValue));
}

void SocialNetworkSyncAdaptor::decrementSemaphore(int accountId)
{
    if (!m_accountSyncSemaphores.contains(accountId)) {
        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: no such semaphore for account: %1")).arg(accountId));
        return;
    }

    int semaphoreValue = m_accountSyncSemaphores.value(accountId);
    semaphoreValue -= 1;
    TRACE(SOCIALD_DEBUG, QString(QLatin1String("decremented busy semaphore for account %1 to %2")).arg(accountId).arg(semaphoreValue));
    if (semaphoreValue < 0) {
        TRACE(SOCIALD_ERROR, QString(QLatin1String("error: busy semaphore is negative for account: %1")).arg(accountId));
        return;
    }
    m_accountSyncSemaphores.insert(accountId, semaphoreValue);

    if (semaphoreValue == 0) {
        finalize(accountId);

        // With the newer implementation, in finalize we can rereaise semaphores,
        // so if after calling finalize, the semaphore count is not the same anymore,
        // we shouldn't update the sync timestamp
        if (m_accountSyncSemaphores.value(accountId) > 0) {
            return;
        }

        // finished all outstanding sync requests for this account.
        // update the sync time in the global sociald database.
        updateLastSyncTimestamp(m_serviceName,
                                SyncService::dataType(dataType), accountId,
                                QDateTime::currentDateTime().toTimeSpec(Qt::UTC));

        // if all outstanding requests for all accounts have finished,
        // then update our status to Inactive / ready to handle more sync requests.
        bool allAreZero = true;
        QList<int> semaphores = m_accountSyncSemaphores.values();
        foreach (int sv, semaphores) {
            if (sv != 0) {
                allAreZero = false;
                break;
            }
        }

        if (allAreZero) {
            setFinishedInactive(); // Finished!
        }
    }
}

QJsonObject SocialNetworkSyncAdaptor::parseJsonObjectReplyData(const QByteArray &replyData, bool *ok)
{
    QJsonDocument jsonDocument = QJsonDocument::fromJson(replyData);
    *ok = !jsonDocument.isEmpty();
    if (*ok && jsonDocument.isObject()) {
        return jsonDocument.object();
    }
    *ok = false;
    return QJsonObject();
}

QJsonArray SocialNetworkSyncAdaptor::parseJsonArrayReplyData(const QByteArray &replyData, bool *ok)
{
    QJsonDocument jsonDocument = QJsonDocument::fromJson(replyData);
    *ok = !jsonDocument.isEmpty();
    if (*ok && jsonDocument.isArray()) {
        return jsonDocument.array();
    }
    *ok = false;
    return QJsonArray();
}
