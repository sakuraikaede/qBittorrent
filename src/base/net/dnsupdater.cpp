/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2011  Christophe Dumez <chris@qbittorrent.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "dnsupdater.h"

#include <QDebug>
#include <QRegularExpression>
#include <QUrlQuery>

#include "base/global.h"
#include "base/logger.h"
#include "base/net/downloadmanager.h"
#include "base/version.h"

using namespace Net;

DNSUpdater::DNSUpdater(QObject *parent)
    : QObject(parent)
    , m_state(OK)
    , m_service(DNS::Service::None)
{
    updateCredentials();

    // Load saved settings from previous session
    const Preferences *const pref = Preferences::instance();
    m_lastIPCheckTime = pref->getDNSLastUpd();
    m_lastIP = QHostAddress(pref->getDNSLastIP());

    // Start IP checking timer
    m_ipCheckTimer.setInterval(IP_CHECK_INTERVAL_MS);
    connect(&m_ipCheckTimer, &QTimer::timeout, this, &DNSUpdater::checkPublicIP);
    m_ipCheckTimer.start();

    // Check lastUpdate to avoid flooding
    if (!m_lastIPCheckTime.isValid()
        || (m_lastIPCheckTime.secsTo(QDateTime::currentDateTime()) * 1000 > IP_CHECK_INTERVAL_MS))
        {
        checkPublicIP();
    }
}

DNSUpdater::~DNSUpdater()
{
    // Save lastupdate time and last ip
    Preferences *const pref = Preferences::instance();
    pref->setDNSLastUpd(m_lastIPCheckTime);
    pref->setDNSLastIP(m_lastIP.toString());
}

void DNSUpdater::checkPublicIP()
{
    Q_ASSERT(m_state == OK);

    DownloadManager::instance()->download(
                DownloadRequest(u"http://checkip.dyndns.org"_qs).userAgent(QStringLiteral("qBittorrent/" QBT_VERSION_2))
                , this, &DNSUpdater::ipRequestFinished);

    m_lastIPCheckTime = QDateTime::currentDateTime();
}

void DNSUpdater::ipRequestFinished(const DownloadResult &result)
{
    if (result.status != DownloadStatus::Success)
    {
        qWarning() << "IP request failed:" << result.errorString;
        return;
    }

    // Parse response
    const QRegularExpressionMatch ipRegexMatch = QRegularExpression(u"Current IP Address:\\s+([^<]+)</body>"_qs).match(QString::fromUtf8(result.data));
    if (ipRegexMatch.hasMatch())
    {
        QString ipStr = ipRegexMatch.captured(1);
        qDebug() << Q_FUNC_INFO << "Regular expression captured the following IP:" << ipStr;
        QHostAddress newIp(ipStr);
        if (!newIp.isNull())
        {
            if (m_lastIP != newIp)
            {
                qDebug() << Q_FUNC_INFO << "The IP address changed, report the change to DynDNS...";
                qDebug() << m_lastIP.toString() << "->" << newIp.toString();
                m_lastIP = newIp;
                updateDNSService();
            }
        }
        else
        {
            qWarning() << Q_FUNC_INFO << "Failed to construct a QHostAddress from the IP string";
        }
    }
    else
    {
        qWarning() << Q_FUNC_INFO << "Regular expression failed to capture the IP address";
    }
}

void DNSUpdater::updateDNSService()
{
    qDebug() << Q_FUNC_INFO;

    m_lastIPCheckTime = QDateTime::currentDateTime();
    DownloadManager::instance()->download(
                DownloadRequest(getUpdateUrl()).userAgent(QStringLiteral("qBittorrent/" QBT_VERSION_2))
                , this, &DNSUpdater::ipUpdateFinished);
}

QString DNSUpdater::getUpdateUrl() const
{
    QUrl url;
#ifdef QT_NO_OPENSSL
    url.setScheme(u"http"_qs);
#else
    url.setScheme(u"https"_qs);
#endif
    url.setUserName(m_username);
    url.setPassword(m_password);

    Q_ASSERT(!m_lastIP.isNull());
    // Service specific
    switch (m_service)
    {
    case DNS::Service::DynDNS:
        url.setHost(u"members.dyndns.org"_qs);
        break;
    case DNS::Service::NoIP:
        url.setHost(u"dynupdate.no-ip.com"_qs);
        break;
    default:
        qWarning() << "Unrecognized Dynamic DNS service!";
        Q_ASSERT(false);
        break;
    }
    url.setPath(u"/nic/update"_qs);

    QUrlQuery urlQuery(url);
    urlQuery.addQueryItem(u"hostname"_qs, m_domain);
    urlQuery.addQueryItem(u"myip"_qs, m_lastIP.toString());
    url.setQuery(urlQuery);
    Q_ASSERT(url.isValid());

    qDebug() << Q_FUNC_INFO << url.toString();
    return url.toString();
}

void DNSUpdater::ipUpdateFinished(const DownloadResult &result)
{
    if (result.status == DownloadStatus::Success)
        processIPUpdateReply(QString::fromUtf8(result.data));
    else
        qWarning() << "IP update failed:" << result.errorString;
}

void DNSUpdater::processIPUpdateReply(const QString &reply)
{
    Logger *const logger = Logger::instance();
    qDebug() << Q_FUNC_INFO << reply;
    const QString code = reply.split(u' ').first();
    qDebug() << Q_FUNC_INFO << "Code:" << code;

    if ((code == u"good") || (code == u"nochg"))
    {
        logger->addMessage(tr("Your dynamic DNS was successfully updated."), Log::INFO);
        return;
    }

    if ((code == u"911") || (code == u"dnserr"))
    {
        logger->addMessage(tr("Dynamic DNS error: The service is temporarily unavailable, it will be retried in 30 minutes."), Log::CRITICAL);
        m_lastIP.clear();
        // It will retry in 30 minutes because the timer was not stopped
        return;
    }

    // Everything below is an error, stop updating until the user updates something
    m_ipCheckTimer.stop();
    m_lastIP.clear();
    if (code == u"nohost")
    {
        logger->addMessage(tr("Dynamic DNS error: hostname supplied does not exist under specified account."), Log::CRITICAL);
        m_state = INVALID_CREDS;
        return;
    }

    if (code == u"badauth")
    {
        logger->addMessage(tr("Dynamic DNS error: Invalid username/password."), Log::CRITICAL);
        m_state = INVALID_CREDS;
        return;
    }

    if (code == u"badagent")
    {
        logger->addMessage(tr("Dynamic DNS error: qBittorrent was blacklisted by the service, please submit a bug report at http://bugs.qbittorrent.org."),
                           Log::CRITICAL);
        m_state = FATAL;
        return;
    }

    if (code == u"!donator")
    {
        logger->addMessage(tr("Dynamic DNS error: %1 was returned by the service, please submit a bug report at http://bugs.qbittorrent.org.").arg(u"!donator"_qs),
                           Log::CRITICAL);
        m_state = FATAL;
        return;
    }

    if (code == u"abuse")
    {
        logger->addMessage(tr("Dynamic DNS error: Your username was blocked due to abuse."), Log::CRITICAL);
        m_state = FATAL;
    }
}

void DNSUpdater::updateCredentials()
{
    if (m_state == FATAL) return;
    Preferences *const pref = Preferences::instance();
    Logger *const logger = Logger::instance();
    bool change = false;
    // Get DNS service information
    if (m_service != pref->getDynDNSService())
    {
        m_service = pref->getDynDNSService();
        change = true;
    }
    if (m_domain != pref->getDynDomainName())
    {
        m_domain = pref->getDynDomainName();
        const QRegularExpressionMatch domainRegexMatch = QRegularExpression(u"^(?:(?!\\d|-)[a-zA-Z0-9\\-]{1,63}\\.)+[a-zA-Z]{2,}$"_qs).match(m_domain);
        if (!domainRegexMatch.hasMatch())
        {
            logger->addMessage(tr("Dynamic DNS error: supplied domain name is invalid."), Log::CRITICAL);
            m_lastIP.clear();
            m_ipCheckTimer.stop();
            m_state = INVALID_CREDS;
            return;
        }
        change = true;
    }
    if (m_username != pref->getDynDNSUsername())
    {
        m_username = pref->getDynDNSUsername();
        if (m_username.length() < 4)
        {
            logger->addMessage(tr("Dynamic DNS error: supplied username is too short."), Log::CRITICAL);
            m_lastIP.clear();
            m_ipCheckTimer.stop();
            m_state = INVALID_CREDS;
            return;
        }
        change = true;
    }
    if (m_password != pref->getDynDNSPassword())
    {
        m_password = pref->getDynDNSPassword();
        if (m_password.length() < 4)
        {
            logger->addMessage(tr("Dynamic DNS error: supplied password is too short."), Log::CRITICAL);
            m_lastIP.clear();
            m_ipCheckTimer.stop();
            m_state = INVALID_CREDS;
            return;
        }
        change = true;
    }

    if ((m_state == INVALID_CREDS) && change)
    {
        m_state = OK; // Try again
        m_ipCheckTimer.start();
        checkPublicIP();
    }
}

QUrl DNSUpdater::getRegistrationUrl(const DNS::Service service)
{
    switch (service)
    {
    case DNS::Service::DynDNS:
        return {u"https://account.dyn.com/entrance/"_qs};
    case DNS::Service::NoIP:
        return {u"https://www.noip.com/remote-access"_qs};
    default:
        Q_ASSERT(false);
        break;
    }
    return {};
}
