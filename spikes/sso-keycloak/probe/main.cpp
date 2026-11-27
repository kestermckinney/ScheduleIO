// SSO spike probe: open a QPSQL connection with a Keycloak token instead of a password.
//
// The token comes from Keycloak's password grant here only to keep the spike
// scriptable; the product would get it from a browser sign-in (PKCE). The token
// reaches the server through libpq's PQsetAuthDataHook, which is process-wide,
// so it also covers the connection Qt's QPSQL driver opens.
//
//   sso_probe <keycloak user> <password> <database role> [token-override]
//   sso_probe --browser <database role>
//
// --browser runs the real desktop sign-in: authorization code + PKCE with a
// loopback redirect. The authorization URL goes to $SSO_PROBE_BROWSER when set
// (the scripted browser used by check.sh), otherwise to the system browser.
#include <QGuiApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrlQuery>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QProcess>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cstdio>
#include <cstring>
#include <dlfcn.h>

// Declarations from PostgreSQL 18's libpq-fe.h (the headers aren't installed on this host).
extern "C" {
typedef struct pg_conn PGconn;
typedef enum { PQAUTHDATA_PROMPT_OAUTH_DEVICE, PQAUTHDATA_OAUTH_BEARER_TOKEN } PGauthData;
typedef struct PGoauthBearerRequest
{
    const char *const openid_configuration;
    const char *const scope;
    int (*async)(PGconn *conn, struct PGoauthBearerRequest *request, int *altsock);
    void (*cleanup)(PGconn *conn, struct PGoauthBearerRequest *request);
    char *token;
    void *user;
} PGoauthBearerRequest;
typedef int (*PQauthDataHook_type)(PGauthData type, PGconn *conn, void *data);
}

static QByteArray g_token;
static int g_hookCalls = 0;

static void freeToken(PGconn *, PGoauthBearerRequest *request)
{
    std::free(request->token);
    request->token = nullptr;
}

static int supplyToken(PGauthData type, PGconn *, void *data)
{
    if (type != PQAUTHDATA_OAUTH_BEARER_TOKEN)
        return 0; // let libpq handle anything else
    auto *request = static_cast<PGoauthBearerRequest *>(data);
    ++g_hookCalls;
    std::fprintf(stderr, "hook: server asks for a token (discovery %s, scope %s)\n",
                 request->openid_configuration, request->scope ? request->scope : "");
    request->token = strdup(g_token.constData());
    request->cleanup = freeToken;
    return 1;
}

static bool installHook()
{
    void *libpq = dlopen("libpq.so.5", RTLD_NOW | RTLD_GLOBAL);
    auto set = libpq ? reinterpret_cast<void (*)(PQauthDataHook_type)>(dlsym(libpq, "PQsetAuthDataHook")) : nullptr;
    if (!set)
        return false;
    set(supplyToken);
    return true;
}

static QByteArray fetchToken(const QString &issuer, const QString &user, const QString &password)
{
    QNetworkAccessManager nam;
    QNetworkRequest request(QUrl(issuer + "/protocol/openid-connect/token"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    QUrlQuery form;
    form.addQueryItem("grant_type", "password");
    form.addQueryItem("client_id", "schedulevault");
    form.addQueryItem("username", user);
    form.addQueryItem("password", password);
    form.addQueryItem("scope", "openid");
    QNetworkReply *reply = nam.post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();
    return body.value("access_token").toString().toUtf8();
}

static QByteArray randomUrlSafe(int bytes)
{
    QByteArray raw(bytes, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(raw.data()), bytes / 4);
    return raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

// Authorization code flow with PKCE (RFC 7636) and a loopback redirect (RFC 8252).
static QByteArray browserSignIn(const QString &issuer)
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0))
        return {};
    const QString redirect = QString("http://127.0.0.1:%1/callback").arg(server.serverPort());
    const QByteArray verifier = randomUrlSafe(48);
    const QByteArray challenge = QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
                                     .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const QByteArray state = randomUrlSafe(24);

    QUrl authorize(issuer + "/protocol/openid-connect/auth");
    QUrlQuery q;
    q.addQueryItem("response_type", "code");
    q.addQueryItem("client_id", "schedulevault");
    q.addQueryItem("redirect_uri", redirect);
    q.addQueryItem("scope", "openid");
    q.addQueryItem("state", QString::fromLatin1(state));
    q.addQueryItem("code_challenge", QString::fromLatin1(challenge));
    q.addQueryItem("code_challenge_method", "S256");
    // Send the person straight to their organization's provider (Keycloak's kc_idp_hint).
    if (const QString hint = qEnvironmentVariable("SSO_PROBE_IDP"); !hint.isEmpty())
        q.addQueryItem("kc_idp_hint", hint);
    authorize.setQuery(q);

    QByteArray code;
    QEventLoop loop;
    QObject::connect(&server, &QTcpServer::newConnection, &loop, [&] {
        QTcpSocket *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, &loop, [&, socket] {
            const QByteArray line = socket->readLine();      // GET /callback?code=..&state=.. HTTP/1.1
            const QUrl url(QString::fromLatin1(line.split(' ').value(1)));
            const QUrlQuery reply(url);
            const bool ok = url.path() == "/callback"
                            && reply.queryItemValue("state").toLatin1() == state
                            && !reply.queryItemValue("code").isEmpty();
            socket->write(QByteArray("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n")
                          + (ok ? "Signed in. You can close this window.\n" : "Sign-in failed.\n"));
            socket->disconnectFromHost();
            if (ok)
                code = reply.queryItemValue("code").toLatin1();
            loop.quit();
        });
    });
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);

    const QString scripted = qEnvironmentVariable("SSO_PROBE_BROWSER");
    if (!scripted.isEmpty())
        QProcess::startDetached(scripted, {authorize.toString(QUrl::FullyEncoded)});
    else if (!QDesktopServices::openUrl(authorize))
        std::fprintf(stderr, "open this address to sign in:\n%s\n", qPrintable(authorize.toString()));
    loop.exec();
    if (code.isEmpty())
        return {};

    QNetworkAccessManager nam;
    QNetworkRequest request(QUrl(issuer + "/protocol/openid-connect/token"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    QUrlQuery form;
    form.addQueryItem("grant_type", "authorization_code");
    form.addQueryItem("client_id", "schedulevault");
    form.addQueryItem("code", QString::fromLatin1(code));
    form.addQueryItem("redirect_uri", redirect);
    form.addQueryItem("code_verifier", QString::fromLatin1(verifier));
    QNetworkReply *reply = nam.post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    QEventLoop wait;
    QObject::connect(reply, &QNetworkReply::finished, &wait, &QEventLoop::quit);
    wait.exec();
    const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();
    std::fprintf(stderr, "browser sign-in: token received (refresh token %s)\n",
                 body.contains("refresh_token") ? "yes" : "no");
    return body.value("access_token").toString().toUtf8();
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments();
    const QString issuer = "http://localhost:8080/realms/vault";
    const bool browser = args.value(1) == "--browser";
    if (browser ? args.size() < 3 : args.size() < 4) {
        std::fprintf(stderr, "usage: sso_probe <user> <password> <role> [token-override]\n"
                             "       sso_probe --browser <role>\n");
        return 2;
    }
    const QString role = browser ? args.at(2) : args.at(3);

    if (browser)
        g_token = browserSignIn(issuer);
    else
        g_token = args.size() > 4 ? args.at(4).toUtf8() : fetchToken(issuer, args.at(1), args.at(2));
    if (g_token.isEmpty()) {
        std::printf("REJECTED (no token): Keycloak did not complete the sign-in\n");
        return 1;
    }
    if (!installHook()) {
        std::fprintf(stderr, "libpq has no PQsetAuthDataHook (needs libpq 18)\n");
        return 1;
    }

    QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL");
    db.setHostName("127.0.0.1");
    db.setPort(5433);
    db.setDatabaseName(qEnvironmentVariable("SSO_PROBE_DATABASE", "spike"));
    db.setUserName(role);
    db.setConnectOptions(QString("oauth_issuer=%1;oauth_client_id=schedulevault").arg(issuer));
    if (!db.open()) {
        std::printf("REJECTED (hook calls %d): %s\n", g_hookCalls,
                    qPrintable(db.lastError().text().simplified()));
        return 3;
    }
    QSqlQuery q(db);
    if (const QString sql = qEnvironmentVariable("SSO_PROBE_SQL"); !sql.isEmpty() && !q.exec(sql)) {
        std::printf("REJECTED by the Vault: %s\n", qPrintable(q.lastError().text().simplified()));
        return 4;
    }
    q.exec("select current_user, system_user");
    q.next();
    std::printf("CONNECTED (hook calls %d): current_user=%s system_user=%s\n", g_hookCalls,
                qPrintable(q.value(0).toString()), qPrintable(q.value(1).toString()));
    return 0;
}
