// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#include "vaultconnection.h"

#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlError>
#include <QUrl>
#include <QUrlQuery>

namespace schedule {

QString VaultConnectionSettings::defaultHost()
{
    return QStringLiteral("localhost");
}

QString VaultConnectionSettings::defaultDatabase()
{
    return QStringLiteral("schedule_vault");
}

bool VaultConnectionSettings::isComplete() const
{
    return !host.trimmed().isEmpty() && !database.trimmed().isEmpty() && !user.trimmed().isEmpty()
        && port >= 1 && port <= 65535;
}

QString VaultConnectionSettings::identity() const
{
    return QStringLiteral("%1\n%2\n%3\n%4").arg(host.trimmed().toLower()).arg(port).arg(database.trimmed(),
                                                                                     user.trimmed());
}

bool VaultConnectionSettings::operator==(const VaultConnectionSettings &other) const
{
    return identity() == other.identity() && verifyTls == other.verifyTls
        && signInIssuer == other.signInIssuer && signInClientId == other.signInClientId
        && signInProvider == other.signInProvider;
}

bool isLocalVaultHost(const QString &host)
{
    const QString name = host.trimmed().toLower();
    return name == QStringLiteral("localhost") || name == QStringLiteral("127.0.0.1") || name == QStringLiteral("::1");
}

bool defaultVerifyTls(const QString &host)
{
    return !isLocalVaultHost(host);
}

QString vaultConnectOptions(bool verifyTls)
{
    return verifyTls ? QStringLiteral("sslmode=verify-full;connect_timeout=5")
                     : QStringLiteral("sslmode=prefer;connect_timeout=5");
}

bool parseVaultConnectionUri(const QString &input, VaultConnectionSettings &settings, QString &password,
                             QString &error)
{
    const QUrl uri(input.trimmed(), QUrl::StrictMode);
    const int port = uri.port(VaultConnectionSettings::DefaultPort);
    const QString host = uri.host(QUrl::FullyDecoded);
    const QString database = uri.path(QUrl::FullyDecoded).mid(1);
    const QString user = uri.userName(QUrl::FullyDecoded);
    const QString secret = uri.password(QUrl::FullyDecoded);
    const auto invalid = [](const QString &part) {
        return part.isEmpty() || part != part.trimmed() || part.contains(QChar::Null);
    };
    if (!uri.isValid() || (uri.scheme() != QStringLiteral("postgresql") && uri.scheme() != QStringLiteral("postgres"))
        || uri.hasFragment() || invalid(host) || host.contains(QLatin1Char(',')) || invalid(database) || invalid(user)
        || secret.contains(QChar::Null) || uri.path(QUrl::FullyEncoded).count(QLatin1Char('/')) != 1
        || port < 1 || port > 65535) {
        error = QCoreApplication::translate("VaultConnection",
                                            "Enter a PostgreSQL URI with one host, a database, and a user.");
        return false;
    }

    bool verifyTls = true;
    bool hasSslMode = false;
    QString issuer, clientId, provider;
    for (const auto &option : QUrlQuery(uri).queryItems(QUrl::FullyDecoded)) {
        QString *signIn = option.first == QStringLiteral("oauth_issuer") ? &issuer
                        : option.first == QStringLiteral("oauth_client_id") ? &clientId
                        : option.first == QStringLiteral("idp") ? &provider : nullptr;
        if (signIn) {
            const bool url = signIn != &issuer || QUrl(option.second, QUrl::StrictMode).scheme().startsWith(QStringLiteral("http"));
            if (!signIn->isEmpty() || option.second.trimmed().isEmpty() || option.second.contains(QChar::Null) || !url) {
                error = QCoreApplication::translate("VaultConnection",
                                                    "oauth_issuer must be an http(s) address, and each sign-in option may appear once.");
                return false;
            }
            *signIn = option.second.trimmed();
            continue;
        }
        if (option.first != QStringLiteral("sslmode") || hasSslMode
            || (option.second != QStringLiteral("verify-full") && option.second != QStringLiteral("prefer"))) {
            error = QCoreApplication::translate("VaultConnection",
                                                "Only sslmode=verify-full or sslmode=prefer is supported.");
            return false;
        }
        hasSslMode = true;
        verifyTls = option.second == QStringLiteral("verify-full");
    }
    if (issuer.isEmpty() && (!clientId.isEmpty() || !provider.isEmpty())) {
        error = QCoreApplication::translate("VaultConnection", "oauth_client_id and idp need oauth_issuer.");
        return false;
    }
    settings.signInIssuer = issuer;
    settings.signInClientId = issuer.isEmpty() ? QString()
                              : clientId.isEmpty() ? VaultConnectionSettings::defaultSignInClientId() : clientId;
    settings.signInProvider = provider;
    settings.host = host;
    settings.port = port;
    settings.database = database;
    settings.user = user;
    settings.verifyTls = verifyTls;
    password = secret;
    return true;
}

QSqlDatabase addVaultDatabase(const QString &connectionName, const VaultConnectionSettings &settings)
{
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), connectionName);
    database.setHostName(settings.host.trimmed());
    database.setPort(settings.port);
    database.setDatabaseName(settings.database.trimmed());
    database.setUserName(settings.user.trimmed());
    QString options = vaultConnectOptions(settings.verifyTls);
    if (settings.usesSingleSignOn()) {
        // libpq's connection-string quoting: values in single quotes.
        const auto quoted = [](QString value) {
            return QLatin1Char('\'') + value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"))
                                             .replace(QLatin1Char('\''), QStringLiteral("\\'")) + QLatin1Char('\'');
        };
        options += QStringLiteral(";oauth_issuer=%1;oauth_client_id=%2")
                       .arg(quoted(settings.signInIssuer.trimmed()),
                            quoted(settings.signInClientId.trimmed().isEmpty()
                                       ? VaultConnectionSettings::defaultSignInClientId()
                                       : settings.signInClientId.trimmed()));
    }
    database.setConnectOptions(options);
    return database;
}

QString vaultConnectionFailure(const QSqlDatabase &database)
{
    const QSqlError error = database.lastError();
    if (error.driverText().contains(QStringLiteral("Driver not loaded"), Qt::CaseInsensitive)
        || !QSqlDatabase::isDriverAvailable(QStringLiteral("QPSQL")))
        return QCoreApplication::translate("VaultConnection", "The Qt PostgreSQL driver (QPSQL) isn’t installed.");
    QString text = error.databaseText().trimmed();
    if (text.isEmpty())
        text = error.driverText().trimmed();
    text = text.section(QLatin1Char('\n'), 0, 0);
    for (const QString &marker : {QStringLiteral("FATAL:"), QStringLiteral("failed:")}) {
        const int at = int(text.lastIndexOf(marker));
        if (at >= 0) {
            text = text.mid(at + marker.size()).trimmed();
            break;
        }
    }
    if (text.isEmpty())
        text = QCoreApplication::translate("VaultConnection", "the server didn’t respond");
    return QCoreApplication::translate("VaultConnection", "Couldn’t connect: %1").arg(text);
}

} // namespace schedule
