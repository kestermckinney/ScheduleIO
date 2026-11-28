// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// The desktop half of single sign-on (vaultsignin.h), against an in-process
// imitation of Supabase Auth and the credential broker: the browser sign-in
// with PKCE, refreshing it, and trading it for a short-lived password. The
// database half is tst_vault_broker.

#include "vault/vaultconnection.h"
#include "vault/vaultsignin.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>
#include <QtTest>

using schedule::VaultConnectionSettings;
using schedule::VaultCredential;
using schedule::VaultSignInTokens;
namespace signin = schedule::vaultSignIn;

namespace {

struct Request
{
    QByteArray method;
    QUrl url;
    QHash<QByteArray, QByteArray> headers;  // lower-case names
    QByteArray body;
};

struct Response
{
    int status = 200;
    QJsonObject body;
};

// A minimal HTTP/1.1 server: one request per connection.
class FakeServer : public QObject
{
public:
    std::function<Response(const Request &)> handle;

    FakeServer()
    {
        m_server.listen(QHostAddress::LocalHost, 0);
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection())
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
        });
    }
    QString address() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

private:
    void read(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        const int end = buffer.indexOf("\r\n\r\n");
        if (end < 0)
            return;
        Request request;
        const QList<QByteArray> lines = buffer.left(end).split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        request.method = first.value(0);
        request.url = QUrl(address() + QString::fromLatin1(first.value(1)));
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const int colon = lines[i].indexOf(':');
            if (colon > 0)
                request.headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
        }
        const int length = request.headers.value("content-length").toInt();
        if (buffer.size() < end + 4 + length)
            return;
        request.body = buffer.mid(end + 4, length);
        m_buffers.remove(socket);
        const Response response = handle(request);
        const QByteArray body = QJsonDocument(response.body).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 " + QByteArray::number(response.status) + " X\r\nContent-Type: application/json\r\n"
                      "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

QByteArray challengeOf(const QByteArray &verifier)
{
    return QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

} // namespace

class VaultSignInTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void signsInThroughTheBrowser();
    void refreshesWithoutTheBrowser();
    void tradesTheSignInForAPassword();
    void refusedSignInReportsWhy();
    void ignoresCallbacksWithoutTheState();

private:
    Response serve(const Request &request);
    // Plays the browser: checks the authorize address, then visits the
    // redirect with the given query (or none, for a stray visit first).
    void useBrowser(const QString &callbackQuery, bool strayFirst = false);

    FakeServer m_server;
    QNetworkAccessManager m_network;
    VaultConnectionSettings m_settings;
    QByteArray m_challenge;
    QString m_lastProvider;
    int m_credentials = 0;
};

void VaultSignInTest::init()
{
    m_server.handle = [this](const Request &request) { return serve(request); };
    m_settings = {};
    m_settings.user = QStringLiteral("ann@acme.example");
    m_settings.signInBroker = m_server.address() + QStringLiteral("/api/vault-credential");
    m_settings.signInProvider = QStringLiteral("azure");
}

Response VaultSignInTest::serve(const Request &request)
{
    const QString path = request.url.path();
    const QUrlQuery query(request.url);
    const QJsonObject body = QJsonDocument::fromJson(request.body).object();
    if (path == QStringLiteral("/api/vault-credential") && request.method == "GET")
        return {200, {{QStringLiteral("supabaseUrl"), m_server.address() + QStringLiteral("/")},
                      {QStringLiteral("anonKey"), QStringLiteral("anon-key")}}};
    if (path == QStringLiteral("/api/vault-credential") && request.method == "POST") {
        const QByteArray token = request.headers.value("authorization");
        if (token == "Bearer access-denied")
            return {403, {{QStringLiteral("error"), QStringLiteral("refused")}}};
        if (token != "Bearer access-1" && token != "Bearer access-2")
            return {401, {{QStringLiteral("error"), QStringLiteral("Sign in again")}}};
        ++m_credentials;
        return {200, {{QStringLiteral("login"), QStringLiteral("ann_example")},
                      {QStringLiteral("password"), QStringLiteral("short-lived-%1").arg(m_credentials)},
                      {QStringLiteral("validUntil"), QStringLiteral("2026-11-27T12:05:00.000Z")}}};
    }
    if (path == QStringLiteral("/auth/v1/token")) {
        if (request.headers.value("apikey") != "anon-key")
            return {401, {{QStringLiteral("msg"), QStringLiteral("No API key found in request")}}};
        if (query.queryItemValue(QStringLiteral("grant_type")) == QStringLiteral("pkce")) {
            const bool ok = body.value(QStringLiteral("auth_code")).toString() == QStringLiteral("code-1")
                            && challengeOf(body.value(QStringLiteral("code_verifier")).toString().toLatin1()) == m_challenge;
            if (!ok)
                return {400, {{QStringLiteral("error_description"), QStringLiteral("code challenge does not match")}}};
            return {200, {{QStringLiteral("access_token"), QStringLiteral("access-1")},
                          {QStringLiteral("refresh_token"), QStringLiteral("refresh-1")},
                          {QStringLiteral("expires_in"), 3600}}};
        }
        if (query.queryItemValue(QStringLiteral("grant_type")) == QStringLiteral("refresh_token")
            && body.value(QStringLiteral("refresh_token")).toString() == QStringLiteral("refresh-1"))
            return {200, {{QStringLiteral("access_token"), QStringLiteral("access-2")},
                          {QStringLiteral("refresh_token"), QStringLiteral("refresh-2")},
                          {QStringLiteral("expires_in"), 3600}}};
        return {400, {{QStringLiteral("error_description"), QStringLiteral("Invalid Refresh Token")}}};
    }
    return {404, {}};
}

void VaultSignInTest::useBrowser(const QString &callbackQuery, bool strayFirst)
{
    signin::setBrowserLauncher([this, callbackQuery, strayFirst](const QUrl &url) {
        const QUrlQuery query(url);
        if (url.toString(QUrl::RemoveQuery) != m_server.address() + QStringLiteral("/auth/v1/authorize"))
            return false;
        m_lastProvider = query.queryItemValue(QStringLiteral("provider"));
        m_challenge = query.queryItemValue(QStringLiteral("code_challenge")).toLatin1();
        if (query.queryItemValue(QStringLiteral("code_challenge_method")) != QStringLiteral("s256"))
            return false;
        const QUrl redirect(query.queryItemValue(QStringLiteral("redirect_to"), QUrl::FullyDecoded));
        if (redirect.host() != QStringLiteral("127.0.0.1") || !redirect.path().startsWith(QStringLiteral("/callback/")))
            return false;
        QTimer::singleShot(0, this, [this, redirect, callbackQuery, strayFirst] {
            if (strayFirst) {
                QUrl stray = redirect;
                stray.setPath(QStringLiteral("/callback/forged"));
                stray.setQuery(QStringLiteral("code=stolen"));
                m_network.get(QNetworkRequest(stray));
            }
            QUrl visit = redirect;
            visit.setQuery(callbackQuery);
            QTimer::singleShot(strayFirst ? 200 : 0, this, [this, visit] { m_network.get(QNetworkRequest(visit)); });
        });
        return true;
    });
}

void VaultSignInTest::signsInThroughTheBrowser()
{
    useBrowser(QStringLiteral("code=code-1"));
    VaultSignInTokens tokens;
    QString error;
    QVERIFY2(signin::signIn(m_settings, tokens, error, 10000), qPrintable(error));
    QCOMPARE(m_lastProvider, QStringLiteral("azure"));
    QCOMPARE(tokens.accessToken, QStringLiteral("access-1"));
    QCOMPARE(tokens.refreshToken, QStringLiteral("refresh-1"));
    QVERIFY(tokens.accessTokenFresh());
}

void VaultSignInTest::refreshesWithoutTheBrowser()
{
    signin::setBrowserLauncher([](const QUrl &) { return false; });
    VaultSignInTokens tokens;
    QString error;
    QVERIFY2(signin::refresh(m_settings, QStringLiteral("refresh-1"), tokens, error), qPrintable(error));
    QCOMPARE(tokens.accessToken, QStringLiteral("access-2"));
    QCOMPARE(tokens.refreshToken, QStringLiteral("refresh-2"));  // Supabase rotates it
    QVERIFY(!signin::refresh(m_settings, QStringLiteral("refresh-1-reused"), tokens, error));
    QVERIFY2(error.contains(QStringLiteral("Invalid Refresh Token")), qPrintable(error));
}

void VaultSignInTest::tradesTheSignInForAPassword()
{
    VaultCredential credential;
    QString error;
    QVERIFY2(signin::fetchCredential(m_settings, QStringLiteral("access-1"), credential, error), qPrintable(error));
    QCOMPARE(credential.login, QStringLiteral("ann_example"));
    QVERIFY(credential.password.startsWith(QStringLiteral("short-lived-")));
    QCOMPARE(credential.validUntil, QDateTime(QDate(2026, 11, 27), QTime(12, 5), QTimeZone::UTC));

    QVERIFY(!signin::fetchCredential(m_settings, QStringLiteral("access-denied"), credential, error));
    QVERIFY2(error.contains(QStringLiteral("doesn’t open a Vault login")), qPrintable(error));
    QVERIFY(!signin::fetchCredential(m_settings, QStringLiteral("expired"), credential, error));
    QCOMPARE(error, QStringLiteral("Sign in again."));
}

void VaultSignInTest::refusedSignInReportsWhy()
{
    useBrowser(QStringLiteral("error=access_denied&error_description=Your%20account%20is%20disabled"));
    VaultSignInTokens tokens;
    QString error;
    QVERIFY(!signin::signIn(m_settings, tokens, error, 10000));
    QVERIFY2(error.contains(QStringLiteral("Your account is disabled")), qPrintable(error));
}

void VaultSignInTest::ignoresCallbacksWithoutTheState()
{
    // A page that guesses the port can't hand in a code: only the one-time
    // path counts, so the real callback after it still signs in.
    useBrowser(QStringLiteral("code=code-1"), true);
    VaultSignInTokens tokens;
    QString error;
    QVERIFY2(signin::signIn(m_settings, tokens, error, 10000), qPrintable(error));
    QCOMPARE(tokens.accessToken, QStringLiteral("access-1"));
}

QTEST_GUILESS_MAIN(VaultSignInTest)
#include "tst_vault_signin.moc"
