// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#include "vaultsignin.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>

#include <cstdlib>
#include <cstring>

// From PostgreSQL 18's libpq-fe.h. The headers needn't be installed: the hook
// is looked up at run time in the libpq the QPSQL driver loaded.
extern "C" {
typedef struct pg_conn PGconn;
typedef enum { PQAUTHDATA_PROMPT_OAUTH_DEVICE, PQAUTHDATA_OAUTH_BEARER_TOKEN } PGauthData;
typedef struct PGoauthBearerRequest
{
    const char *const openid_configuration;
    const char *const scope;
    int (*async)(PGconn *conn, struct PGoauthBearerRequest *request, void *altsock);
    void (*cleanup)(PGconn *conn, struct PGoauthBearerRequest *request);
    char *token;
    void *user;
} PGoauthBearerRequest;
typedef int (*PQauthDataHook_type)(PGauthData type, PGconn *conn, void *data);
}

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

QMutex g_lock;
QHash<QString, QByteArray> g_tokens;  // issuer (no trailing slash) -> access token
PQauthDataHook_type g_previousHook = nullptr;
bool g_installed = false;
BrowserLauncher g_launcher;

QString normalizedIssuer(QString issuer)
{
    issuer = issuer.trimmed();
    while (issuer.endsWith(QLatin1Char('/')))
        issuer.chop(1);
    return issuer;
}

void freeToken(PGconn *, PGoauthBearerRequest *request)
{
    std::free(request->token);
    request->token = nullptr;
}

int supplyToken(PGauthData type, PGconn *conn, void *data)
{
    if (type == PQAUTHDATA_OAUTH_BEARER_TOKEN && data) {
        auto *request = static_cast<PGoauthBearerRequest *>(data);
        // The server names its issuer's discovery document, which lives under
        // the issuer: <issuer>/.well-known/openid-configuration.
        const QString discovery = QString::fromUtf8(request->openid_configuration);
        QByteArray token;
        {
            QMutexLocker locker(&g_lock);
            for (auto it = g_tokens.cbegin(); it != g_tokens.cend(); ++it)
                if (discovery.startsWith(it.key() + QLatin1Char('/')))
                    token = it.value();
        }
        if (!token.isEmpty()) {
            request->token = strdup(token.constData());
            request->cleanup = freeToken;
            return 1;
        }
    }
    return g_previousHook ? g_previousHook(type, conn, data) : 0;
}

QFunctionPointer libpqSymbol(const char *name)
{
#if defined(Q_OS_WIN)
    const QStringList names{QStringLiteral("libpq")};
#elif defined(Q_OS_MACOS)
    const QStringList names{QStringLiteral("libpq.5"), QStringLiteral("pq")};
#else
    const QStringList names{QStringLiteral("libpq.so.5"), QStringLiteral("pq")};
#endif
    for (const QString &library : names) {
        QLibrary libpq(library);
        if (QFunctionPointer symbol = libpq.resolve(name))
            return symbol;  // QLibrary keeps it loaded
    }
    return nullptr;
}

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

QString clientId(const VaultConnectionSettings &settings)
{
    return settings.signInClientId.trimmed().isEmpty() ? VaultConnectionSettings::defaultSignInClientId()
                                                        : settings.signInClientId.trimmed();
}

// The sign-in service's endpoints, from its discovery document.
bool discover(QNetworkAccessManager &network, const QString &issuer, QJsonObject &document, QString &error)
{
    QNetworkReply *reply = network.get(QNetworkRequest(QUrl(issuer + QStringLiteral("/.well-known/openid-configuration"))));
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    loop.exec();
    const bool ok = reply->isFinished() && reply->error() == QNetworkReply::NoError;
    document = QJsonDocument::fromJson(reply->readAll()).object();
    reply->abort();
    reply->deleteLater();
    if (!ok || document.value(QStringLiteral("authorization_endpoint")).toString().isEmpty()) {
        error = tr("Couldn’t reach the sign-in service at %1.").arg(issuer);
        return false;
    }
    return true;
}

bool tokenRequest(QNetworkAccessManager &network, const QString &endpoint, const QUrlQuery &form,
                  VaultSignInTokens &tokens, QString &error)
{
    QNetworkRequest request{QUrl(endpoint)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    QNetworkReply *reply = network.post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    loop.exec();
    const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
    reply->abort();
    reply->deleteLater();
    const QString access = body.value(QStringLiteral("access_token")).toString();
    if (access.isEmpty()) {
        const QString reason = body.value(QStringLiteral("error_description")).toString();
        error = reason.isEmpty() ? tr("The sign-in service didn’t issue a token.")
                                 : tr("The sign-in service didn’t issue a token: %1").arg(reason);
        return false;
    }
    tokens.accessToken = access;
    if (body.contains(QStringLiteral("refresh_token")))
        tokens.refreshToken = body.value(QStringLiteral("refresh_token")).toString();
    const int lifetime = body.value(QStringLiteral("expires_in")).toInt();
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

bool installTokenHook(QString &error)
{
    QMutexLocker locker(&g_lock);
    if (g_installed)
        return true;
    auto set = reinterpret_cast<void (*)(PQauthDataHook_type)>(libpqSymbol("PQsetAuthDataHook"));
    auto get = reinterpret_cast<PQauthDataHook_type (*)()>(libpqSymbol("PQgetAuthDataHook"));
    if (!set || !get) {
        error = tr("Single sign-on needs PostgreSQL 18’s client library (libpq 18 or later).");
        return false;
    }
    g_previousHook = get();
    set(supplyToken);
    g_installed = true;
    return true;
}

void setAccessToken(const QString &issuer, const QString &token)
{
    QMutexLocker locker(&g_lock);
    if (token.isEmpty())
        g_tokens.remove(normalizedIssuer(issuer));
    else
        g_tokens.insert(normalizedIssuer(issuer), token.toUtf8());
}

void setBrowserLauncher(BrowserLauncher launcher)
{
    g_launcher = std::move(launcher);
}

bool signIn(const VaultConnectionSettings &settings, VaultSignInTokens &tokens, QString &error, int timeoutMs)
{
    const QString issuer = normalizedIssuer(settings.signInIssuer);
    QNetworkAccessManager network;
    QJsonObject document;
    if (!discover(network, issuer, document, error))
        return false;

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        error = tr("Couldn’t prepare the sign-in: %1").arg(server.errorString());
        return false;
    }
    const QString redirect = QStringLiteral("http://127.0.0.1:%1/callback").arg(server.serverPort());
    const QByteArray verifier = randomUrlSafe(48);
    const QByteArray challenge = QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
                                     .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const QByteArray state = randomUrlSafe(24);

    QUrl authorize(document.value(QStringLiteral("authorization_endpoint")).toString());
    QUrlQuery query(authorize);
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("client_id"), clientId(settings));
    query.addQueryItem(QStringLiteral("redirect_uri"), redirect);
    query.addQueryItem(QStringLiteral("scope"), QStringLiteral("openid"));
    query.addQueryItem(QStringLiteral("state"), QString::fromLatin1(state));
    query.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(challenge));
    query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    if (!settings.user.trimmed().isEmpty())
        query.addQueryItem(QStringLiteral("login_hint"), settings.user.trimmed());
    // Keycloak: go straight to the organization's identity provider.
    if (!settings.signInProvider.trimmed().isEmpty())
        query.addQueryItem(QStringLiteral("kc_idp_hint"), settings.signInProvider.trimmed());
    authorize.setQuery(query);

    QByteArray code;
    QString refused;
    QEventLoop loop;
    QObject::connect(&server, &QTcpServer::newConnection, &loop, [&] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, &loop, [&, socket] {
                if (!socket->canReadLine())
                    return;
                // GET /callback?code=…&state=… HTTP/1.1
                const QUrl url(QString::fromLatin1(socket->readLine().split(' ').value(1)));
                const QUrlQuery reply(url);
                const bool ours = url.path() == QStringLiteral("/callback")
                                  && reply.queryItemValue(QStringLiteral("state")).toLatin1() == state;
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
                    refused = reply.queryItemValue(QStringLiteral("error_description"),
                                                   QUrl::FullyDecoded).isEmpty()
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

    QUrlQuery form;
    form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
    form.addQueryItem(QStringLiteral("client_id"), clientId(settings));
    form.addQueryItem(QStringLiteral("code"), QString::fromLatin1(code));
    form.addQueryItem(QStringLiteral("redirect_uri"), redirect);
    form.addQueryItem(QStringLiteral("code_verifier"), QString::fromLatin1(verifier));
    tokens = {};
    return tokenRequest(network, document.value(QStringLiteral("token_endpoint")).toString(), form, tokens, error);
}

bool refresh(const VaultConnectionSettings &settings, const QString &refreshToken, VaultSignInTokens &tokens,
             QString &error)
{
    if (refreshToken.isEmpty()) {
        error = tr("Sign in again.");
        return false;
    }
    QNetworkAccessManager network;
    QJsonObject document;
    if (!discover(network, normalizedIssuer(settings.signInIssuer), document, error))
        return false;
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    form.addQueryItem(QStringLiteral("client_id"), clientId(settings));
    form.addQueryItem(QStringLiteral("refresh_token"), refreshToken);
    VaultSignInTokens next;
    next.refreshToken = refreshToken;  // kept unless the service rotates it
    if (!tokenRequest(network, document.value(QStringLiteral("token_endpoint")).toString(), form, next, error))
        return false;
    tokens = next;
    return true;
}

} // namespace vaultSignIn
} // namespace schedule
