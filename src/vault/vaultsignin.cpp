// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#include "vaultsignin.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>

namespace schedule {

bool VaultSignInTokens::accessTokenFresh() const
{
    return !accessToken.isEmpty()
        && (!expiresAt.isValid() || QDateTime::currentDateTimeUtc().secsTo(expiresAt) > 60);
}

namespace vaultSignIn {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("VaultSignIn", text);
}

BrowserLauncher g_launcher;

// The Supabase project a broker trusts: { supabaseUrl, anonKey }.
struct Service
{
    QString url;
    QString key;
};
QMutex g_lock;
QHash<QString, Service> g_services;  // broker address -> its project

bool openInBrowser(const QUrl &url)
{
    if (g_launcher)
        return g_launcher(url);
    // QtCore has no desktop services; apps with a GUI install a launcher.
#if defined(Q_OS_WIN)
    return QProcess::startDetached(QStringLiteral("rundll32"),
                                   {QStringLiteral("url.dll,FileProtocolHandler"), url.toString(QUrl::FullyEncoded)});
#elif defined(Q_OS_MACOS)
    return QProcess::startDetached(QStringLiteral("open"), {url.toString(QUrl::FullyEncoded)});
#else
    return QProcess::startDetached(QStringLiteral("xdg-open"), {url.toString(QUrl::FullyEncoded)});
#endif
}

QByteArray randomUrlSafe(int bytes)
{
    QByteArray raw(bytes, Qt::Uninitialized);
    QRandomGenerator::system()->generate(reinterpret_cast<quint32 *>(raw.data()),
                                         reinterpret_cast<quint32 *>(raw.data() + bytes));
    return raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

struct Reply
{
    int status = 0;
    QJsonObject body;
};

// Sends one request and waits up to 20 seconds for the answer.
Reply send(QNetworkAccessManager &network, QNetworkRequest request, const QByteArray &verb,
           const QByteArray &body = {})
{
    request.setTransferTimeout(20000);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    QNetworkReply *reply = network.sendCustomRequest(request, verb, body);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished())
        loop.exec();
    Reply out;
    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    out.body = QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();
    return out;
}

QString brokerAddress(const VaultConnectionSettings &settings)
{
    return settings.signInBroker.trimmed();
}

bool service(QNetworkAccessManager &network, const VaultConnectionSettings &settings, Service &out, QString &error)
{
    const QString broker = brokerAddress(settings);
    {
        QMutexLocker locker(&g_lock);
        if (g_services.contains(broker)) {
            out = g_services.value(broker);
            return true;
        }
    }
    const Reply reply = send(network, QNetworkRequest(QUrl(broker)), "GET");
    out.url = reply.body.value(QStringLiteral("supabaseUrl")).toString();
    while (out.url.endsWith(QLatin1Char('/')))
        out.url.chop(1);
    out.key = reply.body.value(QStringLiteral("anonKey")).toString();
    if (reply.status != 200 || !isSignInAddress(out.url) || out.key.isEmpty()) {
        error = tr("Couldn’t reach the sign-in service at %1.").arg(broker);
        return false;
    }
    QMutexLocker locker(&g_lock);
    g_services.insert(broker, out);
    return true;
}

QNetworkRequest supabaseRequest(const Service &project, const QString &path)
{
    QNetworkRequest request{QUrl(project.url + path)};
    request.setRawHeader("apikey", project.key.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    return request;
}

bool readTokens(const Reply &reply, VaultSignInTokens &tokens, QString &error)
{
    const QString access = reply.body.value(QStringLiteral("access_token")).toString();
    if (reply.status != 200 || access.isEmpty()) {
        QString reason = reply.body.value(QStringLiteral("error_description")).toString();
        if (reason.isEmpty())
            reason = reply.body.value(QStringLiteral("msg")).toString();
        error = reason.isEmpty() ? tr("The sign-in service didn’t accept the sign-in.")
                                 : tr("The sign-in service didn’t accept the sign-in: %1").arg(reason);
        return false;
    }
    tokens.accessToken = access;
    if (reply.body.contains(QStringLiteral("refresh_token")))
        tokens.refreshToken = reply.body.value(QStringLiteral("refresh_token")).toString();
    const int lifetime = reply.body.value(QStringLiteral("expires_in")).toInt();
    tokens.expiresAt = lifetime > 0 ? QDateTime::currentDateTimeUtc().addSecs(lifetime) : QDateTime();
    return true;
}

const char *page(bool ok)
{
    return ok ? "<!doctype html><meta charset=utf-8><title>Signed in</title>"
                "<p style=\"font:16px system-ui;margin:3em\">You’re signed in. You can close this window and return to Schedule Vault.</p>"
              : "<!doctype html><meta charset=utf-8><title>Sign-in failed</title>"
                "<p style=\"font:16px system-ui;margin:3em\">Sign-in didn’t finish. Return to Schedule Vault and try again.</p>";
}

} // namespace

void setBrowserLauncher(BrowserLauncher launcher)
{
    g_launcher = std::move(launcher);
}

bool signIn(const VaultConnectionSettings &settings, VaultSignInTokens &tokens, QString &error, int timeoutMs)
{
    QNetworkAccessManager network;
    Service project;
    if (!service(network, settings, project, error))
        return false;
    const QString provider = settings.signInProvider.trimmed();
    if (provider.isEmpty()) {
        error = tr("Enter the organization’s sign-in provider.");
        return false;
    }

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        error = tr("Couldn’t prepare the sign-in: %1").arg(server.errorString());
        return false;
    }
    // Supabase keeps redirect_to's path, so the one-time state rides in it.
    const QByteArray state = randomUrlSafe(24);
    const QString callbackPath = QStringLiteral("/callback/") + QString::fromLatin1(state);
    const QString redirect = QStringLiteral("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(callbackPath);
    const QByteArray verifier = randomUrlSafe(48);
    const QByteArray challenge = QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
                                     .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);

    QUrl authorize(project.url + QStringLiteral("/auth/v1/authorize"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("provider"), provider);
    query.addQueryItem(QStringLiteral("redirect_to"), redirect);
    query.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(challenge));
    query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("s256"));
    // Microsoft sends an email address only when asked.
    if (provider == QStringLiteral("azure"))
        query.addQueryItem(QStringLiteral("scopes"), QStringLiteral("email"));
    authorize.setQuery(query);

    QByteArray code;
    QString refused;
    QEventLoop loop;
    QObject::connect(&server, &QTcpServer::newConnection, &loop, [&] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, &loop, [&, socket] {
                if (!socket->canReadLine())
                    return;
                // GET /callback/<state>?code=… HTTP/1.1
                const QUrl url(QString::fromLatin1(socket->readLine().split(' ').value(1)));
                const QUrlQuery reply(url);
                const bool ours = url.path() == callbackPath;
                const bool ok = ours && !reply.queryItemValue(QStringLiteral("code")).isEmpty();
                socket->write(QByteArray("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                                         "Cache-Control: no-store\r\nConnection: close\r\n\r\n") + page(ok));
                socket->disconnectFromHost();
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                if (!ours)
                    return;  // a stray request; keep waiting
                if (ok)
                    code = reply.queryItemValue(QStringLiteral("code")).toLatin1();
                else
                    refused = reply.queryItemValue(QStringLiteral("error_description"), QUrl::FullyDecoded).isEmpty()
                                  ? reply.queryItemValue(QStringLiteral("error"))
                                  : reply.queryItemValue(QStringLiteral("error_description"), QUrl::FullyDecoded);
                loop.quit();
            });
        }
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    if (!openInBrowser(authorize)) {
        error = tr("Couldn’t open the browser to sign in. Open this address: %1").arg(authorize.toString());
        return false;
    }
    loop.exec();
    server.close();
    if (code.isEmpty()) {
        error = !refused.isEmpty() ? tr("Sign-in was cancelled or refused: %1").arg(refused)
                                   : tr("Sign-in didn’t finish in the browser.");
        return false;
    }

    const QJsonObject body{{QStringLiteral("auth_code"), QString::fromLatin1(code)},
                           {QStringLiteral("code_verifier"), QString::fromLatin1(verifier)}};
    tokens = {};
    return readTokens(send(network, supabaseRequest(project, QStringLiteral("/auth/v1/token?grant_type=pkce")), "POST",
                           QJsonDocument(body).toJson(QJsonDocument::Compact)),
                      tokens, error);
}

bool refresh(const VaultConnectionSettings &settings, const QString &refreshToken, VaultSignInTokens &tokens,
             QString &error)
{
    if (refreshToken.isEmpty()) {
        error = tr("Sign in again.");
        return false;
    }
    QNetworkAccessManager network;
    Service project;
    if (!service(network, settings, project, error))
        return false;
    const QJsonObject body{{QStringLiteral("refresh_token"), refreshToken}};
    VaultSignInTokens next;
    if (!readTokens(send(network, supabaseRequest(project, QStringLiteral("/auth/v1/token?grant_type=refresh_token")),
                         "POST", QJsonDocument(body).toJson(QJsonDocument::Compact)),
                    next, error))
        return false;
    tokens = next;
    return true;
}

bool fetchCredential(const VaultConnectionSettings &settings, const QString &accessToken,
                     VaultCredential &credential, QString &error)
{
    QNetworkAccessManager network;
    QNetworkRequest request{QUrl(brokerAddress(settings))};
    request.setRawHeader("Authorization", "Bearer " + accessToken.toUtf8());
    const Reply reply = send(network, request, "POST");
    const QString login = reply.body.value(QStringLiteral("login")).toString();
    const QString password = reply.body.value(QStringLiteral("password")).toString();
    if (reply.status == 200 && !login.isEmpty() && !password.isEmpty()) {
        credential.login = login;
        credential.password = password;
        credential.validUntil = QDateTime::fromString(reply.body.value(QStringLiteral("validUntil")).toString(),
                                                      Qt::ISODateWithMs);
        return true;
    }
    const QString reason = reply.body.value(QStringLiteral("error")).toString();
    switch (reply.status) {
    case 401:
        error = tr("Sign in again.");
        break;
    case 403:
        error = tr("Your organization’s sign-in doesn’t open a Vault login here. Ask your Vault administrator to "
                   "check your sign-in email.");
        break;
    case 429:
        error = reason.isEmpty() ? tr("Too many sign-ins. Wait a minute and try again.") : reason;
        break;
    default:
        error = tr("Couldn’t reach the sign-in service at %1.").arg(brokerAddress(settings));
    }
    return false;
}

} // namespace vaultSignIn
} // namespace schedule
