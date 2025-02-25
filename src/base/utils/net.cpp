/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2016  Alexandr Milovantsev <dzmat@yandex.ru>
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

#include "net.h"

#include <QList>
#include <QNetworkInterface>
#include <QPair>
#include <QSslCertificate>
#include <QSslKey>
#include <QString>
#include <QVector>

#include "base/global.h"

namespace Utils
{
    namespace Net
    {
        bool isValidIP(const QString &ip)
        {
            return !QHostAddress(ip).isNull();
        }

        Subnet parseSubnet(const QString &subnetStr, bool *ok)
        {
            const Subnet invalid = qMakePair(QHostAddress(), -1);
            const Subnet subnet = QHostAddress::parseSubnet(subnetStr);
            if (ok)
                *ok = (subnet != invalid);
            return subnet;
        }

        bool canParseSubnet(const QString &subnetStr)
        {
            bool ok = false;
            parseSubnet(subnetStr, &ok);
            return ok;
        }

        bool isLoopbackAddress(const QHostAddress &addr)
        {
            return (addr == QHostAddress::LocalHost)
                    || (addr == QHostAddress::LocalHostIPv6)
                    || (addr == QHostAddress(u"::ffff:127.0.0.1"_qs));
        }

        bool isIPInRange(const QHostAddress &addr, const QVector<Subnet> &subnets)
        {
            QHostAddress protocolEquivalentAddress;
            bool addrConversionOk = false;

            if (addr.protocol() == QAbstractSocket::IPv4Protocol)
            {
                // always succeeds
                protocolEquivalentAddress = QHostAddress(addr.toIPv6Address());
                addrConversionOk = true;
            }
            else
            {
                // only succeeds when addr is an ipv4-mapped ipv6 address
                protocolEquivalentAddress = QHostAddress(addr.toIPv4Address(&addrConversionOk));
            }

            for (const Subnet &subnet : subnets)
                if (addr.isInSubnet(subnet) || (addrConversionOk && protocolEquivalentAddress.isInSubnet(subnet)))
                    return true;

            return false;
        }

        QString subnetToString(const Subnet &subnet)
        {
            return subnet.first.toString() + u'/' + QString::number(subnet.second);
        }

        QHostAddress canonicalIPv6Addr(const QHostAddress &addr)
        {
            // Link-local IPv6 textual address always contains a scope id (or zone index)
            // The scope id is appended to the IPv6 address using the '%' character
            // The scope id can be either a interface name or an interface number
            // Examples:
            // fe80::1%ethernet_17
            // fe80::1%13
            // The interface number is the mandatory supported way
            // Unfortunately for us QHostAddress::toString() outputs (at least on Windows)
            // the interface name, and libtorrent/boost.asio only support an interface number
            // as scope id. Furthermore, QHostAddress doesn't have any convenient method to
            // affect this, so we jump through hoops here.
            if (addr.protocol() != QAbstractSocket::IPv6Protocol)
                return QHostAddress{addr.toIPv6Address()};

            // QHostAddress::setScopeId(addr.scopeId()); // Even though the docs say that setScopeId
            // will convert a name to a number, this doesn't happen. Probably a Qt bug.
            const QString scopeIdTxt = addr.scopeId();
            if (scopeIdTxt.isEmpty())
                return addr;

            const int id = QNetworkInterface::interfaceIndexFromName(scopeIdTxt);
            if (id == 0) // This failure might mean that the scope id was already a number
                return addr;

            QHostAddress canonical(addr.toIPv6Address());
            canonical.setScopeId(QString::number(id));
            return canonical;
        }

        QList<QSslCertificate> loadSSLCertificate(const QByteArray &data)
        {
            const QList<QSslCertificate> certs {QSslCertificate::fromData(data)};
            if (std::any_of(certs.cbegin(), certs.cend(), [](const QSslCertificate &c) { return c.isNull(); }))
                return {};
            return certs;
        }

        bool isSSLCertificatesValid(const QByteArray &data)
        {
            return !loadSSLCertificate(data).isEmpty();
        }

        QSslKey loadSSLKey(const QByteArray &data)
        {
            // try different formats
            QSslKey key {data, QSsl::Rsa};
            if (!key.isNull())
                return key;
            return {data, QSsl::Ec};
        }

        bool isSSLKeyValid(const QByteArray &data)
        {
            return !loadSSLKey(data).isNull();
        }
    }
}
