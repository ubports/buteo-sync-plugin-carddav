/*
 * This file is part of buteo-sync-plugin-carddav package
 *
 * Copyright (C) 2014 Jolla Ltd. and/or its subsidiary(-ies).
 *
 * Contributors: Chris Adams <chris.adams@jolla.com>
 *
 * This program/library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This program/library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program/library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include "carddav_p.h"
#include "syncer_p.h"

#include <LogMacros.h>

#include <QRegularExpression>
#include <QUuid>
#include <QByteArray>
#include <QBuffer>
#include <QTimer>

#include <QContact>
#include <QContactGuid>
#include <QContactAvatar>
#include <QContactDisplayLabel>
#include <QContactName>
#include <QContactNickname>
#include <QContactBirthday>
#include <QContactTimestamp>
#include <QContactGender>

#include <QVersitWriter>
#include <QVersitDocument>
#include <QVersitProperty>
#include <QVersitContactExporter>
#include <QVersitReader>
#include <QVersitContactImporter>

#include <seasidepropertyhandler.h>
#include <seasidecache.h>

#include <qtcontacts-extensions.h>

namespace {
    void debugDumpData(const QString &data)
    {
        if (Buteo::Logger::instance()->getLogLevel() < 7) {
            return;
        }

        QString dbgout;
        Q_FOREACH (const QChar &c, data) {
            if (c == '\r' || c == '\n') {
                if (!dbgout.isEmpty()) {
                    LOG_DEBUG(dbgout);
                    dbgout.clear();
                }
            } else {
                dbgout += c;
            }
        }
        if (!dbgout.isEmpty()) {
            LOG_DEBUG(dbgout);
        }
    }
}

CardDavVCardConverter::CardDavVCardConverter()
{
}

CardDavVCardConverter::~CardDavVCardConverter()
{
}

QStringList CardDavVCardConverter::supportedPropertyNames()
{
    // We only support a small number of (core) vCard properties
    // in this sync adapter.  The rest of the properties will
    // be cached so that we can stitch them back into the vCard
    // we upload on modification.
    QStringList supportedProperties;
    supportedProperties << "VERSION" << "PRODID" << "REV"
                        << "N" << "FN" << "NICKNAME" << "BDAY" << "X-GENDER"
                        << "EMAIL" << "TEL" << "ADR" << "URL" << "PHOTO"
                        << "ORG" << "TITLE" << "ROLE"
                        << "UID";
    return supportedProperties;
}

QPair<QContact, QStringList> CardDavVCardConverter::convertVCardToContact(const QString &vcard, bool *ok)
{
    m_unsupportedProperties.clear();
    QVersitReader reader(vcard.toUtf8());
    reader.startReading();
    reader.waitForFinished();
    QList<QVersitDocument> vdocs = reader.results();
    if (vdocs.size() != 1) {
        LOG_WARNING(Q_FUNC_INFO
                   << "invalid results during vcard import, got"
                   << vdocs.size() << "output from input:\n" << vcard);
        *ok = false;
        return QPair<QContact, QStringList>();
    }

    // convert the vCard into a QContact
    QVersitContactImporter importer;
    importer.setPropertyHandler(this);
    importer.importDocuments(vdocs);
    QList<QContact> importedContacts = importer.contacts();
    if (importedContacts.size() != 1) {
        LOG_WARNING(Q_FUNC_INFO
                   << "invalid results during vcard conversion, got"
                   << importedContacts.size() << "output from input:\n" << vcard);
        *ok = false;
        return QPair<QContact, QStringList>();
    }

    QContact importedContact = importedContacts.first();
    QStringList unsupportedProperties = m_unsupportedProperties.value(importedContact.detail<QContactGuid>().guid());
    m_unsupportedProperties.clear();

    // If the contact has no structured name data, create a best-guess name for it.
    // This may be the case if the server provides an FN property but no N property.
    // Also, some detail types should be unique, so remove duplicates if present.
    QString displaylabelField, nicknameField;
    QContactName nameDetail;
    QSet<QContactDetail::DetailType> seenUniqueDetailTypes;
    QList<QContactDetail> importedContactDetails = importedContact.details();
    Q_FOREACH (const QContactDetail &d, importedContactDetails) {
        if (d.type() == QContactDetail::TypeName) {
            nameDetail = d;
        } else if (d.type() == QContactDetail::TypeDisplayLabel) {
            displaylabelField = d.value(QContactDisplayLabel::FieldLabel).toString().trimmed();
        } else if (d.type() == QContactDetail::TypeNickname) {
            nicknameField = d.value(QContactNickname::FieldNickname).toString().trimmed();
        } else if (d.type() == QContactDetail::TypeBirthday) {
            if (seenUniqueDetailTypes.contains(QContactDetail::TypeBirthday)) {
                // duplicated BDAY field seen from vCard.
                // remove this duplicate, else save will fail.
                QContactBirthday dupBday(d);
                importedContact.removeDetail(&dupBday);
                LOG_DEBUG("Removed duplicate BDAY detail:" << dupBday);
            } else {
                seenUniqueDetailTypes.insert(QContactDetail::TypeBirthday);
            }
        } else if (d.type() == QContactDetail::TypeTimestamp) {
            if (seenUniqueDetailTypes.contains(QContactDetail::TypeTimestamp)) {
                // duplicated REV field seen from vCard.
                // remove this duplicate, else save will fail.
                QContactTimestamp dupRev(d);
                importedContact.removeDetail(&dupRev);
                LOG_DEBUG("Removed duplicate REV detail:" << dupRev);
            } else {
                seenUniqueDetailTypes.insert(QContactDetail::TypeTimestamp);
            }
        } else if (d.type() == QContactDetail::TypeGuid) {
            if (seenUniqueDetailTypes.contains(QContactDetail::TypeGuid)) {
                // duplicated UID field seen from vCard.
                // remove this duplicate, else save will fail.
                QContactGuid dupUid(d);
                importedContact.removeDetail(&dupUid);
                LOG_DEBUG("Removed duplicate UID detail:" << dupUid);
            } else {
                seenUniqueDetailTypes.insert(QContactDetail::TypeGuid);
            }
        } else if (d.type() == QContactDetail::TypeGender) {
            if (seenUniqueDetailTypes.contains(QContactDetail::TypeGender)) {
                // duplicated X-GENDER field seen from vCard.
                // remove this duplicate, else save will fail.
                QContactGender dupGender(d);
                importedContact.removeDetail(&dupGender);
                LOG_DEBUG("Removed duplicate X-GENDER detail:" << dupGender);
            } else {
                seenUniqueDetailTypes.insert(QContactDetail::TypeGender);
            }
        }
    }
    if (nameDetail.isEmpty() || (nameDetail.firstName().isEmpty() && nameDetail.lastName().isEmpty())) {
        // we have no valid name data but we may have display label or nickname data which we can decompose.
        if (!displaylabelField.isEmpty()) {
            SeasideCache::decomposeDisplayLabel(displaylabelField, &nameDetail);
            importedContact.saveDetail(&nameDetail);
            LOG_DEBUG("Decomposed vCard display name into structured name:" << nameDetail);
        } else if (!nicknameField.isEmpty()) {
            SeasideCache::decomposeDisplayLabel(nicknameField, &nameDetail);
            importedContact.saveDetail(&nameDetail);
            LOG_DEBUG("Decomposed vCard nickname into structured name:" << nameDetail);
        } else {
            LOG_WARNING("No structured name data exists in the vCard, contact will be unnamed!");
        }
    }

    // mark each detail of the contact as modifiable
    Q_FOREACH (QContactDetail det, importedContact.details()) {
        det.setValue(QContactDetail__FieldModifiable, true);
        importedContact.saveDetail(&det);
    }

    *ok = true;
    return qMakePair(importedContact, unsupportedProperties);
}

QString CardDavVCardConverter::convertContactToVCard(const QContact &c, const QStringList &unsupportedProperties)
{
    QList<QContact> exportList; exportList << c;
    QVersitContactExporter e;
    e.setDetailHandler(this);
    e.exportContacts(exportList);
    QByteArray output;
    QBuffer vCardBuffer(&output);
    vCardBuffer.open(QBuffer::WriteOnly);
    QVersitWriter writer(&vCardBuffer);
    writer.startWriting(e.documents());
    writer.waitForFinished();
    QString retn = QString::fromUtf8(output);

    // now add back the unsupported properties.
    Q_FOREACH (const QString &propStr, unsupportedProperties) {
        int endIdx = retn.lastIndexOf(QStringLiteral("END:VCARD"));
        if (endIdx > 0) {
            QString ecrlf = propStr + '\r' + '\n';
            retn.insert(endIdx, ecrlf);
        }
    }

    LOG_DEBUG("generated vcard:");
    debugDumpData(retn);

    return retn;
}

QString CardDavVCardConverter::convertPropertyToString(const QVersitProperty &p) const
{
    QVersitDocument d(QVersitDocument::VCard30Type);
    d.addProperty(p);
    QByteArray out;
    QBuffer bout(&out);
    bout.open(QBuffer::WriteOnly);
    QVersitWriter w(&bout);
    w.startWriting(d);
    w.waitForFinished();
    QString retn = QString::fromLatin1(out);

    // strip out the BEGIN:VCARD\r\nVERSION:3.0\r\n and END:VCARD\r\n\r\n bits.
    int headerIdx = retn.indexOf(QStringLiteral("VERSION:3.0")) + 11;
    int footerIdx = retn.indexOf(QStringLiteral("END:VCARD"));
    if (headerIdx > 11 && footerIdx > 0 && footerIdx > headerIdx) {
        retn = retn.mid(headerIdx, footerIdx - headerIdx).trimmed();
        return retn;
    }

    LOG_WARNING(Q_FUNC_INFO << "no string conversion possible for versit property:" << p.name());
    return QString();
}

void CardDavVCardConverter::propertyProcessed(const QVersitDocument &, const QVersitProperty &property,
                                               const QContact &, bool *alreadyProcessed,
                                               QList<QContactDetail> *updatedDetails)
{
    static QStringList supportedProperties(supportedPropertyNames());
    const QString propertyName(property.name().toUpper());
    if (propertyName == QLatin1String("PHOTO")) {
        // use the standard PHOTO handler from Seaside libcontacts
        QContactAvatar newAvatar = SeasidePropertyHandler::avatarFromPhotoProperty(property);
        if (!newAvatar.isEmpty()) {
            updatedDetails->append(newAvatar);
        }
        // don't let the default PHOTO handler import it, even if we failed above.
        *alreadyProcessed = true;
        return;
    } else if (supportedProperties.contains(propertyName)) {
        // do nothing, let the default handler import them.
        *alreadyProcessed = true;
        return;
    }

    // cache the unsupported property string, and remove any detail
    // which was added by the default handler for this property.
    *alreadyProcessed = true;
    QString unsupportedProperty = convertPropertyToString(property);
    m_tempUnsupportedProperties.append(unsupportedProperty);
    updatedDetails->clear();
}

void CardDavVCardConverter::documentProcessed(const QVersitDocument &, QContact *c)
{
    // the UID of the contact will be contained in the QContactGuid detail.
    QString uid = c->detail<QContactGuid>().guid();
    if (uid.isEmpty()) {
        LOG_WARNING(Q_FUNC_INFO << "imported contact has no UID, discarding unsupported properties!");
    } else {
        m_unsupportedProperties.insert(uid, m_tempUnsupportedProperties);
    }

    // get ready for the next import.
    m_tempUnsupportedProperties.clear();
}

void CardDavVCardConverter::contactProcessed(const QContact &c, QVersitDocument *d)
{
    // FN is a required field in vCard 3.0 and 4.0.  Add it if it does not exist.
    bool foundFN = false;
    Q_FOREACH (const QVersitProperty &p, d->properties()) {
        if (p.name() == QStringLiteral("FN")) {
            foundFN = true;
            break;
        }
    }

    // N is also a required field in vCard 3.0.  Add it if it does not exist.
    bool foundN = false;
    Q_FOREACH (const QVersitProperty &p, d->properties()) {
        if (p.name() == QStringLiteral("N")) {
            foundN = true;
            break;
        }
    }

    if (!foundFN || !foundN) {
        QString displaylabel = SeasideCache::generateDisplayLabel(c);
        if (!foundFN) {
            QVersitProperty fnProp;
            fnProp.setName("FN");
            fnProp.setValue(displaylabel);
            d->addProperty(fnProp);
        }
        if (!foundN) {
            QContactName name = c.detail<QContactName>();
            SeasideCache::decomposeDisplayLabel(displaylabel, &name);
            if (name.firstName().isEmpty()) {
                // If we could not decompose the display label (e.g., only one token)
                // then just assume that the display label is a useful first name.
                name.setFirstName(displaylabel);
            }
            QString nvalue = QStringLiteral("%1;%2;;;").arg(name.lastName(), name.firstName());
            QVersitProperty nProp;
            nProp.setName("N");
            nProp.setValue(nvalue);
            d->addProperty(nProp);
        }
    }
}

void CardDavVCardConverter::detailProcessed(const QContact &, const QContactDetail &,
                                            const QVersitDocument &, QSet<int> *,
                                            QList<QVersitProperty> *, QList<QVersitProperty> *toBeAdded)
{
    static QStringList supportedProperties(supportedPropertyNames());
    for (int i = toBeAdded->size() - 1; i >= 0; --i) {
        const QString propName = toBeAdded->at(i).name().toUpper();
        if (!supportedProperties.contains(propName)) {
            // we don't support importing these properties, so we shouldn't
            // attempt to export them.
            toBeAdded->removeAt(i);
        } else if (propName == QStringLiteral("X-GENDER")
                && toBeAdded->at(i).value().toUpper() == QStringLiteral("UNSPECIFIED")) {
            // this is probably added "by default" since qtcontacts-sqlite always stores a gender.
            toBeAdded->removeAt(i);
        }
    }
}

CardDav::CardDav(Syncer *parent,
                 const QString &serverUrl,
                 const QString &addressbookPath,
                 const QString &username,
                 const QString &password)
    : QObject(parent)
    , q(parent)
    , m_converter(new CardDavVCardConverter)
    , m_request(new RequestGenerator(q, username, password))
    , m_parser(new ReplyParser(q, m_converter))
    , m_serverUrl(serverUrl)
    , m_addressbookPath(addressbookPath)
    , m_discoveryStage(CardDav::DiscoveryStarted)
    , m_addressbooksListOnly(false)
    , m_triedAddressbookPathAsHomeSetUrl(false)
    , m_downsyncRequests(0)
    , m_upsyncRequests(0)
{
}

CardDav::CardDav(Syncer *parent,
                 const QString &serverUrl,
                 const QString &addressbookPath,
                 const QString &accessToken)
    : QObject(parent)
    , q(parent)
    , m_converter(new CardDavVCardConverter)
    , m_request(new RequestGenerator(q, accessToken))
    , m_parser(new ReplyParser(q, m_converter))
    , m_serverUrl(serverUrl)
    , m_addressbookPath(addressbookPath)
    , m_discoveryStage(CardDav::DiscoveryStarted)
    , m_addressbooksListOnly(false)
    , m_downsyncRequests(0)
    , m_upsyncRequests(0)
{
}

CardDav::~CardDav()
{
    delete m_converter;
    delete m_parser;
    delete m_request;
}

void CardDav::errorOccurred(int httpError)
{
    emit error(httpError);
}

void CardDav::determineAddressbooksList()
{
    m_addressbooksListOnly = true;
    determineRemoteAMR();
}

void CardDav::determineRemoteAMR()
{
    if (m_addressbookPath.isEmpty()) {
        // The CardDAV sequence for determining the A/M/R delta is:
        // a)  fetch user information from the principal URL
        // b)  fetch addressbooks home url
        // c)  fetch addressbook information
        // d)  for each addressbook, either:
        //     i)  perform immediate delta sync (if webdav-sync enabled) OR
        //     ii) fetch etags, manually calculate delta
        // e) fetch full contacts for delta.

        // We start by fetching user information.
        fetchUserInformation();
    } else {
        // we can skip to step (c) of the discovery.
        fetchAddressbooksInformation(m_addressbookPath);
    }
}

void CardDav::fetchUserInformation()
{
    LOG_DEBUG(Q_FUNC_INFO << "requesting principal urls for user");

    // we need to specify the .well-known/carddav endpoint if it's the first
    // request (so we have not yet been redirected to the correct endpoint)
    // and if the path is empty/unknown.

    /*
        RFC 6764 section 6.5:

        * The client does a "PROPFIND" [RFC4918] request with the
          request URI set to the initial "context path".  The body of
          the request SHOULD include the DAV:current-user-principal
          [RFC5397] property as one of the properties to return.  Note
          that clients MUST properly handle HTTP redirect responses for
          the request.  The server will use the HTTP authentication
          procedure outlined in [RFC2617] or use some other appropriate
          authentication schemes to authenticate the user.

        * When an initial "context path" has not been determined from a
          TXT record, the initial "context path" is taken to be
          "/.well-known/caldav" (for CalDAV) or "/.well-known/carddav"
          (for CardDAV).

        * If the server returns a 404 ("Not Found") HTTP status response
          to the request on the initial "context path", clients MAY try
          repeating the request on the "root" URI "/" or prompt the user
          for a suitable path.
    */

    QUrl serverUrl(m_serverUrl);
    QString wellKnownUrl = QStringLiteral("%1://%2/.well-known/carddav").arg(serverUrl.scheme()).arg(serverUrl.host());
    bool firstRequest = m_discoveryStage == CardDav::DiscoveryStarted;
    m_serverUrl = firstRequest && (serverUrl.path().isEmpty() || serverUrl.path() == QStringLiteral("/"))
                ? wellKnownUrl
                : m_serverUrl;
    QNetworkReply *reply = m_request->currentUserInformation(m_serverUrl);
    if (!reply) {
        emit error();
        return;
    }

    connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsOccurred(QList<QSslError>)));
    connect(reply, SIGNAL(finished()), this, SLOT(userInformationResponse()));
}

void CardDav::sslErrorsOccurred(const QList<QSslError> &errors)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (q->m_ignoreSslErrors) {
        LOG_DEBUG(Q_FUNC_INFO << "ignoring SSL errors due to account policy:" << errors);
        reply->ignoreSslErrors(errors);
    } else {
        LOG_WARNING(Q_FUNC_INFO << "SSL errors occurred, aborting:" << errors);
        errorOccurred(401);
    }
}

void CardDav::userInformationResponse()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        int httpError = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        LOG_WARNING(Q_FUNC_INFO << "error:" << reply->error() << "(" << httpError << ") to request" << m_serverUrl);
        debugDumpData(QString::fromUtf8(data));
        QUrl oldServerUrl(m_serverUrl);
        if (m_discoveryStage == CardDav::DiscoveryStarted && (httpError == 404 || httpError == 405)) {
            if (!oldServerUrl.path().endsWith(QStringLiteral(".well-known/carddav"))) {
                // From RFC 6764: If the initial "context path" derived from a TXT record
                // generates HTTP errors when targeted by requests, the client
                // SHOULD repeat its "bootstrapping" procedure using the
                // appropriate ".well-known" URI instead.
                LOG_DEBUG(Q_FUNC_INFO << "got HTTP response" << httpError << "to initial discovery request; trying well-known URI");
                m_serverUrl = QStringLiteral("%1://%2/.well-known/carddav").arg(oldServerUrl.scheme()).arg(oldServerUrl.host());
                fetchUserInformation(); // set initial context path to well-known URI.
            } else {
                // From RFC 6764: if the server returns a 404 HTTP status response to the
                // request on the initial context path, clients may try repeating the request
                // on the root URI.
                // We also do this on HTTP 405 in case some implementation is non-spec-conformant.
                LOG_DEBUG(Q_FUNC_INFO << "got HTTP response" << httpError << "to well-known request; trying root URI");
                m_discoveryStage = CardDav::DiscoveryTryRoot;
                m_serverUrl = QStringLiteral("%1://%2/").arg(oldServerUrl.scheme()).arg(oldServerUrl.host());
                fetchUserInformation();
            }
            return;
        }
        errorOccurred(httpError);
        return;
    }

    // if the request was to the /.well-known/carddav path, then we need to redirect
    QUrl redir = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if (!redir.isEmpty()) {
        QUrl orig = reply->url();
        if (orig.path() != redir.path()) {
            if (orig.path().endsWith(QStringLiteral(".well-known/carddav"))) {
                // redirect as required, and change our server URL to point to the redirect URL.
                LOG_DEBUG(Q_FUNC_INFO << "redirecting from:" << orig.toString() << "to:" << redir.toString());
                m_serverUrl = QStringLiteral("%1://%2%3")
                        .arg(redir.scheme().isEmpty() ? orig.scheme() : redir.scheme())
                        .arg(redir.host().isEmpty() ? orig.host() : redir.host())
                        .arg(redir.path());
                m_discoveryStage = CardDav::DiscoveryRedirected;
                fetchUserInformation();
            } else {
                // possibly unsafe redirect.  for security, assume it's malicious and abort sync.
                LOG_WARNING(Q_FUNC_INFO << "unexpected redirect from:" << orig.toString() << "to:" << redir.toString());
                errorOccurred(301);
            }
        } else {
            // circular redirect, avoid the endless loop by aborting sync.
            LOG_WARNING(Q_FUNC_INFO << "redirect specified is circular:" << redir.toString());
            errorOccurred(301);
        }
        return;
    }

    ReplyParser::ResponseType responseType = ReplyParser::UserPrincipalResponse;
    QString userPath = m_parser->parseUserPrincipal(data, &responseType);
    if (responseType == ReplyParser::UserPrincipalResponse) {
        // the server responded with the expected user principal information.
        if (userPath.isEmpty()) {
            LOG_WARNING(Q_FUNC_INFO << "unable to parse user principal from response");
            emit error();
            return;
        }
        fetchAddressbookUrls(userPath);
    } else if (responseType == ReplyParser::AddressbookInformationResponse) {
        // the server responded with addressbook information instead
        // of user principal information.  Skip the next discovery step.
        QList<ReplyParser::AddressBookInformation> infos = m_parser->parseAddressbookInformation(data, QString());
        if (infos.isEmpty()) {
            LOG_WARNING(Q_FUNC_INFO << "unable to parse addressbook info from user principal response");
            emit error();
            return;
        }
        downsyncAddressbookContent(infos);
    } else {
        LOG_WARNING(Q_FUNC_INFO << "unknown response from user principal request");
        emit error();
    }
}

void CardDav::fetchAddressbookUrls(const QString &userPath)
{
    LOG_DEBUG(Q_FUNC_INFO << "requesting addressbook urls for user");
    QNetworkReply *reply = m_request->addressbookUrls(m_serverUrl, userPath);
    if (!reply) {
        emit error();
        return;
    }

    connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsOccurred(QList<QSslError>)));
    connect(reply, SIGNAL(finished()), this, SLOT(addressbookUrlsResponse()));
}

void CardDav::addressbookUrlsResponse()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        int httpError = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        LOG_WARNING(Q_FUNC_INFO << "error:" << reply->error()
                   << "(" << httpError << ")");
        debugDumpData(QString::fromUtf8(data));
        errorOccurred(httpError);
        return;
    }

    QString addressbooksHomePath = m_parser->parseAddressbookHome(data);
    if (addressbooksHomePath.isEmpty()) {
        LOG_WARNING(Q_FUNC_INFO << "unable to parse addressbook home from response");
        emit error();
        return;
    }

    fetchAddressbooksInformation(addressbooksHomePath);
}

void CardDav::fetchAddressbooksInformation(const QString &addressbooksHomePath)
{
    LOG_DEBUG(Q_FUNC_INFO << "requesting addressbook sync information");
    QNetworkReply *reply = m_request->addressbooksInformation(m_serverUrl, addressbooksHomePath);
    reply->setProperty("addressbooksHomePath", addressbooksHomePath);
    if (!reply) {
        emit error();
        return;
    }

    connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsOccurred(QList<QSslError>)));
    connect(reply, SIGNAL(finished()), this, SLOT(addressbooksInformationResponse()));
}

void CardDav::addressbooksInformationResponse()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QString addressbooksHomePath = reply->property("addressbooksHomePath").toString();
    QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        int httpError = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        LOG_WARNING(Q_FUNC_INFO << "error:" << reply->error()
                   << "(" << httpError << ")");
        debugDumpData(QString::fromUtf8(data));
        errorOccurred(httpError);
        return;
    }

    // if we didn't parse the addressbooks home path via discovery, but instead were provided it by the user,
    // then don't pass the path to the parser, as it uses it for cycle detection.
    if (m_addressbookPath == addressbooksHomePath) {
        addressbooksHomePath = QString();
    }

    QList<ReplyParser::AddressBookInformation> infos = m_parser->parseAddressbookInformation(data, addressbooksHomePath);
    if (infos.isEmpty()) {
        if (!m_addressbookPath.isEmpty() && !m_triedAddressbookPathAsHomeSetUrl) {
            // the user provided an addressbook path during account creation, which didn't work.
            // it may not be an addressbook path but instead the home set url; try that.
            LOG_DEBUG(Q_FUNC_INFO << "Given path is not addressbook path; trying as home set url");
            m_triedAddressbookPathAsHomeSetUrl = true;
            fetchAddressbookUrls(m_addressbookPath);
        } else {
            LOG_WARNING(Q_FUNC_INFO << "unable to parse addressbook info from response");
            emit error();
            return;
        }
    }

    if (m_addressbooksListOnly) {
        QStringList paths;
        for (QList<ReplyParser::AddressBookInformation>::const_iterator it = infos.constBegin(); it != infos.constEnd(); ++it) {
            if (!paths.contains(it->url)) {
                paths.append(it->url);
            }
        }
        emit addressbooksList(paths);
    } else {
        downsyncAddressbookContent(infos);
    }
}

void CardDav::downsyncAddressbookContent(const QList<ReplyParser::AddressBookInformation> &infos)
{
    // for addressbooks which support sync-token syncing, use that style.
    for (int i = 0; i < infos.size(); ++i) {
        // set a default addressbook if we haven't seen one yet.
        // we will store newly added local contacts to that addressbook.
        if (q->m_defaultAddressbook.isEmpty()) {
            q->m_defaultAddressbook = infos[i].url;
        }

        if (infos[i].syncToken.isEmpty() && infos[i].ctag.isEmpty()) {
            // we cannot use either sync-token or ctag for this addressbook.
            // we need to manually calculate the complete delta.
            LOG_DEBUG("No sync-token or ctag given for addressbook:" << infos[i].url << ", manual delta detection required");
            q->m_addressbookCtags[infos[i].url] = infos[i].ctag; // ctag is empty :. we will use manual detection.
            fetchContactMetadata(infos[i].url);
        } else if (infos[i].syncToken.isEmpty()) {
            // we cannot use sync-token for this addressbook, but instead ctag.
            const QString existingCtag(q->m_addressbookCtags[infos[i].url]); // from OOB
            if (existingCtag.isEmpty()) {
                // first time sync
                q->m_addressbookCtags[infos[i].url] = infos[i].ctag; // insert
                // now do etag request, the delta will be all remote additions
                fetchContactMetadata(infos[i].url);
            } else if (existingCtag != infos[i].ctag) {
                // changes have occurred since last sync
                q->m_addressbookCtags[infos[i].url] = infos[i].ctag; // update
                // perform etag request and then manually calculate deltas.
                fetchContactMetadata(infos[i].url);
            } else {
                // no changes have occurred in this addressbook since last sync
                LOG_DEBUG(Q_FUNC_INFO << "no changes since last sync for"
                         << infos[i].url << "from account" << q->m_accountId);
                m_downsyncRequests += 1;
                QTimer::singleShot(0, this, SLOT(downsyncComplete()));
            }
        } else {
            // the server supports webdav-sync for this addressbook.
            const QString existingSyncToken(q->m_addressbookSyncTokens[infos[i].url]); // from OOB
            // store the ctag anyway just in case the server has
            // forgotten the syncToken we cached from last time.
            if (!infos[i].ctag.isEmpty()) {
                q->m_addressbookCtags[infos[i].url] = infos[i].ctag;
            }
            // attempt to perform synctoken sync
            if (existingSyncToken.isEmpty()) {
                // first time sync
                q->m_addressbookSyncTokens[infos[i].url] = infos[i].syncToken; // insert
                // perform slow sync / full report
                fetchContactMetadata(infos[i].url);
            } else if (existingSyncToken != infos[i].syncToken) {
                // changes have occurred since last sync.
                q->m_addressbookSyncTokens[infos[i].url] = infos[i].syncToken; // update
                // perform immediate delta sync, by passing the old sync token to the server.
                fetchImmediateDelta(infos[i].url, existingSyncToken);
            } else {
                // no changes have occurred in this addressbook since last sync
                LOG_DEBUG(Q_FUNC_INFO << "no changes since last sync for"
                         << infos[i].url << "from account" << q->m_accountId);
                m_downsyncRequests += 1;
                QTimer::singleShot(0, this, SLOT(downsyncComplete()));
            }
        }
    }
}

void CardDav::fetchImmediateDelta(const QString &addressbookUrl, const QString &syncToken)
{
    LOG_DEBUG(Q_FUNC_INFO
             << "requesting immediate delta for addressbook" << addressbookUrl
             << "with sync token" << syncToken);

    QNetworkReply *reply = m_request->syncTokenDelta(m_serverUrl, addressbookUrl, syncToken);
    if (!reply) {
        emit error();
        return;
    }

    m_downsyncRequests += 1; // when this reaches zero, we've finished all addressbook deltas
    reply->setProperty("addressbookUrl", addressbookUrl);
    connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsOccurred(QList<QSslError>)));
    connect(reply, SIGNAL(finished()), this, SLOT(immediateDeltaResponse()));
}

void CardDav::immediateDeltaResponse()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QString addressbookUrl = reply->property("addressbookUrl").toString();
    QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        LOG_WARNING(Q_FUNC_INFO << "error:" << reply->error()
                   << "(" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() << ")");
        debugDumpData(QString::fromUtf8(data));
        // The server is allowed to forget the syncToken by the
        // carddav protocol.  Try a full report sync just in case.
        fetchContactMetadata(addressbookUrl);
        return;
    }

    QString newSyncToken;
    QList<ReplyParser::ContactInformation> infos = m_parser->parseSyncTokenDelta(data, &newSyncToken);
    q->m_addressbookSyncTokens[addressbookUrl] = newSyncToken;
    fetchContacts(addressbookUrl, infos);
}

void CardDav::fetchContactMetadata(const QString &addressbookUrl)
{
    LOG_DEBUG(Q_FUNC_INFO << "requesting contact metadata for addressbook" << addressbookUrl);
    QNetworkReply *reply = m_request->contactEtags(m_serverUrl, addressbookUrl);
    if (!reply) {
        emit error();
        return;
    }

    m_downsyncRequests += 1; // when this reaches zero, we've finished all addressbook deltas
    reply->setProperty("addressbookUrl", addressbookUrl);
    connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsOccurred(QList<QSslError>)));
    connect(reply, SIGNAL(finished()), this, SLOT(contactMetadataResponse()));
}

void CardDav::contactMetadataResponse()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QString addressbookUrl = reply->property("addressbookUrl").toString();
    QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        int httpError = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        LOG_WARNING(Q_FUNC_INFO << "error:" << reply->error()
                   << "(" << httpError << ")");
        debugDumpData(QString::fromUtf8(data));
        errorOccurred(httpError);
        return;
    }

    QList<ReplyParser::ContactInformation> infos = m_parser->parseContactMetadata(data, addressbookUrl);
    fetchContacts(addressbookUrl, infos);
}

void CardDav::fetchContacts(const QString &addressbookUrl, const QList<ReplyParser::ContactInformation> &amrInfo)
{
    LOG_DEBUG(Q_FUNC_INFO << "requesting full contact information from addressbook" << addressbookUrl);

    // split into A/M/R request sets
    QStringList contactUris;
    Q_FOREACH (const ReplyParser::ContactInformation &info, amrInfo) {
        if (info.modType == ReplyParser::ContactInformation::Addition) {
            q->m_serverAdditionIndices[addressbookUrl].insert(info.uri, q->m_serverAdditions[addressbookUrl].size());
            q->m_serverAdditions[addressbookUrl].append(info);
            contactUris.append(info.uri);
        } else if (info.modType == ReplyParser::ContactInformation::Modification) {
            q->m_serverModificationIndices[addressbookUrl].insert(info.uri, q->m_serverModifications[addressbookUrl].size());
            q->m_serverModifications[addressbookUrl].append(info);
            contactUris.append(info.uri);
        } else if (info.modType == ReplyParser::ContactInformation::Deletion) {
            q->m_serverDeletions[addressbookUrl].append(info);
        } else {
            LOG_WARNING(Q_FUNC_INFO << "no modification type in info for:" << info.uri);
        }
    }

    LOG_DEBUG(Q_FUNC_INFO << "Have calculated AMR:"
             << q->m_serverAdditions[addressbookUrl].size()
             << q->m_serverModifications[addressbookUrl].size()
             << q->m_serverDeletions[addressbookUrl].size()
             << "for addressbook:" << addressbookUrl);

    if (contactUris.isEmpty()) {
        // no additions or modifications to fetch.
        LOG_DEBUG(Q_FUNC_INFO << "no further data to fetch");
        contactAddModsComplete(addressbookUrl);
    } else {
        // fetch the full contact data for additions/modifications.
        LOG_DEBUG(Q_FUNC_INFO << "fetching vcard data for" << contactUris.size() << "contacts");
        QNetworkReply *reply = m_request->contactMultiget(m_serverUrl, addressbookUrl, contactUris);
        if (!reply) {
            emit error();
            return;
        }

        reply->setProperty("addressbookUrl", addressbookUrl);
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsOccurred(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(contactsResponse()));
    }
}

void CardDav::contactsResponse()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QString addressbookUrl = reply->property("addressbookUrl").toString();
    QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        int httpError = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        LOG_WARNING(Q_FUNC_INFO << "error:" << reply->error()
                   << "(" << httpError << ")");
        debugDumpData(QString::fromUtf8(data));
        errorOccurred(httpError);
        return;
    }

    QList<QContact> added;
    QList<QContact> modified;

    // fill out added/modified.  Also keep our addressbookContactGuids state up-to-date.
    // The addMods map is a map from server contact uri to <contact/unsupportedProperties/etag>.
    QMap<QString, ReplyParser::FullContactInformation> addMods = m_parser->parseContactData(data, addressbookUrl);
    QMap<QString, ReplyParser::FullContactInformation>::const_iterator it = addMods.constBegin();
    for ( ; it != addMods.constEnd(); ++it) {
        if (q->m_serverAdditionIndices[addressbookUrl].contains(it.key())) {
            QContact c = it.value().contact;
            QString guid = c.detail<QContactGuid>().guid();
            q->m_serverAdditions[addressbookUrl][q->m_serverAdditionIndices[addressbookUrl].value(it.key())].guid = guid;
            q->m_contactEtags[guid] = it.value().etag;
            q->m_contactUris[guid] = it.key();
            q->m_contactUnsupportedProperties[guid] = it.value().unsupportedProperties;
            // Note: for additions, q->m_contactUids will have been filled out by the reply parser.
            q->m_addressbookContactGuids[addressbookUrl].append(guid);
            // Check to see if this server-side addition is actually just
            // a reported previously-upsynced local-side addition.
            if (q->m_contactIds.contains(guid)) {
                QContact previouslyUpsynced = c;
                previouslyUpsynced.setId(QContactId::fromString(q->m_contactIds[guid]));
                added.append(previouslyUpsynced);
            } else {
                // pure server-side addition.
                added.append(c);
            }
            q->m_serverAddModsByUid.insert(q->m_contactUids[guid], qMakePair(addressbookUrl, c));
        } else if (q->m_serverModificationIndices[addressbookUrl].contains(it.key())) {
            QContact c = it.value().contact;
            QString guid = c.detail<QContactGuid>().guid();
            q->m_contactUnsupportedProperties[guid] = it.value().unsupportedProperties;
            q->m_contactEtags[guid] = it.value().etag;
            if (!q->m_contactIds.contains(guid)) {
                LOG_WARNING(Q_FUNC_INFO << "modified contact has no id");
            } else {
                c.setId(QContactId::fromString(q->m_contactIds[guid]));
            }
            modified.append(c);
            q->m_serverAddModsByUid.insert(q->m_contactUids[guid], qMakePair(addressbookUrl, c));
        } else {
            LOG_WARNING(Q_FUNC_INFO << "ignoring unknown addition/modification:" << it.key());
        }
    }

    // coalesce the added/modified contacts from this addressbook into the complete AMR
    m_remoteAdditions.append(added);
    m_remoteModifications.append(modified);

    // now handle removals
    contactAddModsComplete(addressbookUrl);
}

void CardDav::contactAddModsComplete(const QString &addressbookUrl)
{
    QList<QContact> removed;

    // fill out removed set, and remove any state data associated with removed contacts
    for (int i = 0; i < q->m_serverDeletions[addressbookUrl].size(); ++i) {
        QString guid = q->m_serverDeletions[addressbookUrl][i].guid;
        if (!q->m_contactIds.contains(guid)) {
            // check to see if we have an entry which matches the "old" guid form.
            // if so, use the "old" guid form instead.
            QString prefix = QStringLiteral("%1:AB:%2:").arg(QString::number(q->m_accountId), addressbookUrl);
            if (guid.startsWith(prefix)) {
                guid = QStringLiteral("%1:%2").arg(QString::number(q->m_accountId), guid.mid(prefix.length()));
            }
        }

        // create the contact to remove
        QContact doomed;
        QContactGuid cguid;
        cguid.setGuid(guid);
        doomed.saveDetail(&cguid);
        if (!q->m_contactIds.contains(guid)) {
            LOG_WARNING(Q_FUNC_INFO << "removed contact has no id");
            continue; // cannot remove it if we don't know the id
        }
        doomed.setId(QContactId::fromString(q->m_contactIds[guid]));
        removed.append(doomed);

        // update the state data
        q->m_contactUids.remove(guid);
        q->m_contactUris.remove(guid);
        q->m_contactEtags.remove(guid);
        q->m_contactIds.remove(guid);
        q->m_contactUnsupportedProperties.remove(guid);
        q->m_addressbookContactGuids[addressbookUrl].removeOne(guid);
    }

    // coalesce the removed contacts from this addressbook into the complete AMR
    m_remoteRemovals.append(removed);

    // downsync complete for this addressbook.
    // we use a singleshot to ensure that the m_deltaRequests count isn't
    // decremented synchronously to zero if the first addressbook didn't
    // have any remote additions or modifications (requiring async request).
    QTimer::singleShot(0, this, SLOT(downsyncComplete()));
}

void CardDav::downsyncComplete()
{
    // downsync complete for this addressbook
    // if this was the last outstanding addressbook, we're finished.
    m_downsyncRequests -= 1;
    if (m_downsyncRequests == 0) {
        LOG_DEBUG(Q_FUNC_INFO
                 << "downsync complete with total AMR:"
                 << m_remoteAdditions.size() << ","
                 << m_remoteModifications.size() << ","
                 << m_remoteRemovals.size());
        emit remoteChanges(m_remoteAdditions, m_remoteModifications, m_remoteRemovals);
    }
}

static QString transformIntoAddressbookSpecificGuid(const QString &guidstr, int accountId, const QString &addressbookUrl)
{
    QString retn;
    if (guidstr.startsWith(QStringLiteral("%1:AB:%2:").arg(QString::number(accountId), addressbookUrl))) {
        // nothing to do, already a guid for this addressbook
        return guidstr;
    } else if (guidstr.startsWith(QStringLiteral("%1:AB:").arg(accountId))) {
        // guid for a different addressbook.
        LOG_WARNING("error: guid for different addressbook:" << guidstr);
        return guidstr; // return it anyway, rather than attempt to mangle it with this addressbookUrl also.
    } else {
        // transform into addressbook-url style GUID.
        if (guidstr.startsWith(QStringLiteral("%1:").arg(accountId))) {
            // already accountId prefixed (e.g., from a previous sync cycle prior to when we supported addressbookUrl-prefixed-guids
            retn = QStringLiteral("%1:AB:%2:%3").arg(QString::number(accountId), addressbookUrl, guidstr.mid(guidstr.indexOf(':')+1));
        } else {
            // non-prefixed, device-side guid (e.g., a local contact addition)
            retn = QStringLiteral("%1:AB:%2:%3").arg(QString::number(accountId), addressbookUrl, guidstr);
        }
    }
    return retn;
}

static void setUpsyncContactGuid(QContact *c, const QString &uid)
{
    // in the case where the exact same contact is contained
    // in multiple remote addressbooks, the syncContact generated
    // locally may contain duplicated GUID data.  Filter these out
    // and instead set the UID as the guid field for upsync.
    QList<QContactGuid> guids = c->details<QContactGuid>();
    for (int i = guids.size(); i > 1; --i) {
        QContactGuid &g(guids[i-1]);
        c->removeDetail(&g);
    }

    QContactGuid newGuid = c->detail<QContactGuid>();
    newGuid.setGuid(uid);
    c->saveDetail(&newGuid);
}

void CardDav::upsyncUpdates(const QString &addressbookUrl, const QList<QContact> &added, const QList<QContact> &modified, const QList<QContact> &removed)
{
    LOG_DEBUG(Q_FUNC_INFO
             << "upsyncing updates to addressbook:" << addressbookUrl
             << ":" << added.count() << modified.count() << removed.count());

    bool hadNonSpuriousChanges = false;
    int spuriousModifications = 0;

    // put local additions
    for (int i = 0; i < added.size(); ++i) {
        QContact c = added.at(i);
        // generate a server-side uid
        QString uid = QUuid::createUuid().toString().replace(QRegularExpression(QStringLiteral("[\\-{}]")), QString());
        // transform into local-device guid
        QString guid = QStringLiteral("%1:AB:%2:%3").arg(QString::number(q->m_accountId), addressbookUrl, uid);
        // generate a valid uri
        QString uri = addressbookUrl + "/" + uid + ".vcf";
        // update our state data
        q->m_contactUids[guid] = uid;
        q->m_contactUris[guid] = uri;
        q->m_contactIds[guid] = c.id().toString();
        // set the uid not guid so that the VCF UID is generated.
        setUpsyncContactGuid(&c, uid);
        // generate a vcard
        QString vcard = m_converter->convertContactToVCard(c, QStringList());
        // upload
        QNetworkReply *reply = m_request->upsyncAddMod(m_serverUrl, uri, QString(), vcard);
        if (!reply) {
            emit error();
            return;
        }

        m_upsyncRequests += 1;
        hadNonSpuriousChanges = true;
        reply->setProperty("addressbookUrl", addressbookUrl);
        reply->setProperty("contactGuid", guid);
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsOccurred(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(upsyncResponse()));
    }

    // put local modifications
    for (int i = 0; i < modified.size(); ++i) {
        QContact c = modified.at(i);
        // reinstate the server-side UID into the guid detail
        QString guidstr = c.detail<QContactGuid>().guid();
        if (guidstr.isEmpty()) {
            LOG_WARNING(Q_FUNC_INFO << "modified contact has no guid:" << c.id().toString());
            continue; // TODO: this is actually an error.
        }
        QString oldguidstr = guidstr;
        guidstr = transformIntoAddressbookSpecificGuid(guidstr, q->m_accountId, addressbookUrl);
        QString uidstr = q->m_contactUids[guidstr];
        if (uidstr.isEmpty()) {
            // check to see if the old guid was used previously.
            // this should only occur after the package upgrade, and not normally.
            if (!q->m_contactUids.value(oldguidstr).isEmpty()) {
                q->migrateGuidData(oldguidstr, guidstr, addressbookUrl);
                uidstr = q->m_contactUids.value(guidstr);
            } else {
                LOG_WARNING(Q_FUNC_INFO << "modified contact server uid unknown:" << c.id().toString() << guidstr);
                continue; // TODO: this is actually an error.
            }
        }
        setUpsyncContactGuid(&c, uidstr);
        // now check to see if it's a spurious change caused by downsync of a remote addition/modification
        // perhaps to the same contact in a different addressbook.
        if (q->m_serverAddModsByUid.contains(uidstr)) {
            bool spurious = true;
            const QList<QPair<QString, QContact> > &addressbookContacts(q->m_serverAddModsByUid.values(uidstr));
            for (int j = 0; j < addressbookContacts.size(); ++j) {
                QContact downsyncedContact = addressbookContacts[j].second;
                if (q->significantDifferences(&c, &downsyncedContact)) {
                    spurious = false;
                    break;
                }
            }
            if (spurious) {
                LOG_DEBUG(Q_FUNC_INFO << "not upsyncing spurious change to contact:" << guidstr);
                spuriousModifications += 1;
                continue;
            }
        }
        // otherwise, convert to vcard and upsync to remote server.
        QString vcard = m_converter->convertContactToVCard(c, q->m_contactUnsupportedProperties[guidstr]);
        // upload
        QNetworkReply *reply = m_request->upsyncAddMod(m_serverUrl,
                q->m_contactUris[guidstr],
                q->m_contactEtags[guidstr],
                vcard);
        if (!reply) {
            emit error();
            return;
        }

        m_upsyncRequests += 1;
        hadNonSpuriousChanges = true;
        reply->setProperty("addressbookUrl", addressbookUrl);
        reply->setProperty("contactGuid", guidstr);
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsOccurred(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(upsyncResponse()));
    }

    // delete local removals
    for (int i = 0; i < removed.size(); ++i) {
        QContact c = removed[i];
        QString guidstr = c.detail<QContactGuid>().guid();
        QString oldguidstr = guidstr;
        guidstr = transformIntoAddressbookSpecificGuid(guidstr, q->m_accountId, addressbookUrl);
        if (q->m_contactUris.value(guidstr).isEmpty()) {
            // check to see if the old guid was used previously.
            // this should only occur after the package upgrade, and not normally.
            if (!q->m_contactUris.value(oldguidstr).isEmpty()) {
                q->migrateGuidData(oldguidstr, guidstr, addressbookUrl);
            } else {
                LOG_WARNING(Q_FUNC_INFO << "deleted contact server uri unknown:" << c.id().toString() << guidstr);
                continue; // TODO: this is actually an error.
            }
        }
        QNetworkReply *reply = m_request->upsyncDeletion(m_serverUrl,
                q->m_contactUris[guidstr],
                q->m_contactEtags[guidstr]);
        if (!reply) {
            emit error();
            return;
        }

        // clear state data for this (deleted) contact
        q->m_contactEtags.remove(guidstr);
        q->m_contactUris.remove(guidstr);
        q->m_contactIds.remove(guidstr);
        q->m_contactUids.remove(guidstr);
        q->m_addressbookContactGuids[addressbookUrl].removeOne(guidstr);

        m_upsyncRequests += 1;
        hadNonSpuriousChanges = true;
        reply->setProperty("addressbookUrl", addressbookUrl);
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsOccurred(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(upsyncResponse()));
    }

    if (!hadNonSpuriousChanges || (added.size() == 0 && modified.size() == 0 && removed.size() == 0)) {
        // nothing to upsync.  Use a singleshot to avoid synchronously
        // decrementing the m_upsyncRequests count to zero if there
        // happens to be nothing to upsync to the first addressbook.
        m_upsyncRequests += 1;
        QTimer::singleShot(0, this, SLOT(upsyncComplete()));
    }

    LOG_DEBUG(Q_FUNC_INFO << "ignored" << spuriousModifications << "spurious updates to addressbook:" << addressbookUrl);
}

void CardDav::upsyncResponse()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QString guid = reply->property("contactGuid").toString();
    QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        int httpError = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        LOG_WARNING(Q_FUNC_INFO << "error:" << reply->error()
                   << "(" << httpError << ")");
        debugDumpData(QString::fromUtf8(data));
        if (httpError == 405) {
            // MethodNotAllowed error.  Most likely the server has restricted
            // new writes to the collection (e.g., read-only or update-only).
            // We should not abort the sync if we receive this error.
            LOG_WARNING(Q_FUNC_INFO << "405 MethodNotAllowed - is the collection read-only?");
            LOG_WARNING(Q_FUNC_INFO << "continuing sync despite this error - upsync will have failed!");
        } else {
            errorOccurred(httpError);
            return;
        }
    }

    if (!guid.isEmpty()) {
        // this is an addition or modification.
        // get the new etag value reported by the server.
        QString etag;
        Q_FOREACH(const QByteArray &header, reply->rawHeaderList()) {
            if (QString::fromUtf8(header).contains(QLatin1String("etag"), Qt::CaseInsensitive)) {
                etag = reply->rawHeader(header);
                break;
            }
        }

        if (!etag.isEmpty()) {
            LOG_DEBUG("Got updated etag for" << guid << ":" << etag);
            q->m_contactEtags[guid] = etag;
        } else {
            // If we don't perform an additional request, the etag server-side will be different to the etag
            // we have locally, and thus on next sync we would spuriously detect a server-side modification.
            // That's ok, we'll just detect that it's spurious via data inspection during the next sync.
            LOG_WARNING("No updated etag provided for" << guid << ": will be reported as spurious remote modification next sync");
        }
    }

    // upsync is complete for this addressbook.
    upsyncComplete();
}

void CardDav::upsyncComplete()
{
    m_upsyncRequests -= 1;
    if (m_upsyncRequests == 0) {
        // finished upsyncing all data for all addressbooks.
        LOG_DEBUG(Q_FUNC_INFO << "upsync complete");
        emit upsyncCompleted();
    }
}
