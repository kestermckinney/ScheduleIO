// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Single sign-on end to end, against the Keycloak spike in
// spikes/sso-keycloak (./up.sh): the browser sign-in (played by the spike's
// scripted browser), the libpq token hook, and refreshing the token. Runs only
// when SCHEDULEIO_SSO_SPIKE_DIR names that folder.

#include "vault/vaultconnection.h"
#include "vault/vaultsignin.h"

#include <QProcess>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QtTest>

using schedule::VaultConnectionSettings;
using schedule::VaultSignInTokens;

class VaultSignInTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void signsInThroughTheBrowser();
    void refreshesWithoutTheBrowser();
    void refusedSignInReportsWhy();

private:
    VaultConnectionSettings settings(const QString &user) const;
    QString open(const VaultConnectionSettings &settings);
    void useScriptedBrowser(const QString &user, const QString &password);

    QString m_spike;
    VaultSignInTokens m_tokens;
};

void VaultSignInTest::initTestCase()
{
    m_spike = qEnvironmentVariable("SCHEDULEIO_SSO_SPIKE_DIR");
    if (m_spike.isEmpty())
        QSKIP("Set SCHEDULEIO_SSO_SPIKE_DIR to spikes/sso-keycloak with ./up.sh running.");
    qputenv("PGOAUTHDEBUG", "UNSAFE");  // the spike's Keycloak is plain HTTP
    QString error;
    if (!schedule::vaultSignIn::installTokenHook(error))
        QSKIP(qPrintable(error));
}

VaultConnectionSettings VaultSignInTest::settings(const QString &user) const
{
    VaultConnectionSettings result;
    result.host = QStringLiteral("127.0.0.1");
    result.port = 5433;
    result.database = QStringLiteral("spike");
    result.user = user;
    result.verifyTls = false;
    result.signInIssuer = QStringLiteral("http://localhost:8080/realms/vault");
    return result;
}

QString VaultSignInTest::open(const VaultConnectionSettings &connection)
{
    const QString name = QStringLiteral("tst_vault_signin");
    QString result;
    {
        QSqlDatabase database = schedule::addVaultDatabase(name, connection);
        if (!database.open(connection.user, QString())) {
            result = schedule::vaultConnectionFailure(database);
        } else {
            QSqlQuery query(database);
            query.exec(QStringLiteral("SELECT current_user"));
            query.next();
            result = query.value(0).toString();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return result;
}

void VaultSignInTest::useScriptedBrowser(const QString &user, const QString &password)
{
    const QString script = m_spike + QStringLiteral("/scripted-browser.sh");
    schedule::vaultSignIn::setBrowserLauncher([script, user, password](const QUrl &url) {
        auto *browser = new QProcess;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("SSO_USER"), user);
        environment.insert(QStringLiteral("SSO_PASSWORD"), password);
        browser->setProcessEnvironment(environment);
        QObject::connect(browser, &QProcess::finished, browser, &QObject::deleteLater);
        browser->start(script, {url.toString(QUrl::FullyEncoded)});
        return browser->waitForStarted();
    });
}

void VaultSignInTest::signsInThroughTheBrowser()
{
    useScriptedBrowser(QStringLiteral("svt_alice"), QStringLiteral("testuser1"));
    const VaultConnectionSettings alice = settings(QStringLiteral("svt_alice"));
    QString error;
    QVERIFY2(schedule::vaultSignIn::signIn(alice, m_tokens, error, 60000), qPrintable(error));
    QVERIFY(m_tokens.accessTokenFresh());
    QVERIFY(!m_tokens.refreshToken.isEmpty());

    // No token handed to the hook: the server refuses.
    QVERIFY(open(alice) != QStringLiteral("svt_alice"));
    schedule::vaultSignIn::setAccessToken(alice.signInIssuer + QLatin1Char('/'), m_tokens.accessToken);
    QCOMPARE(open(alice), QStringLiteral("svt_alice"));
    // The token opens only Alice's login.
    QVERIFY(open(settings(QStringLiteral("svt_bob"))) != QStringLiteral("svt_bob"));
}

void VaultSignInTest::refreshesWithoutTheBrowser()
{
    if (m_tokens.refreshToken.isEmpty())
        QSKIP("Needs signsInThroughTheBrowser.");
    schedule::vaultSignIn::setBrowserLauncher([](const QUrl &) { return false; });
    const VaultConnectionSettings alice = settings(QStringLiteral("svt_alice"));
    VaultSignInTokens next;
    QString error;
    QVERIFY2(schedule::vaultSignIn::refresh(alice, m_tokens.refreshToken, next, error), qPrintable(error));
    QVERIFY(next.accessToken != m_tokens.accessToken);
    schedule::vaultSignIn::setAccessToken(alice.signInIssuer, next.accessToken);
    QCOMPARE(open(alice), QStringLiteral("svt_alice"));

    QVERIFY(!schedule::vaultSignIn::refresh(alice, QStringLiteral("not-a-refresh-token"), next, error));
    QVERIFY(!error.isEmpty());
}

void VaultSignInTest::refusedSignInReportsWhy()
{
    useScriptedBrowser(QStringLiteral("svt_alice"), QStringLiteral("wrong-password"));
    VaultSignInTokens tokens;
    QString error;
    QVERIFY(!schedule::vaultSignIn::signIn(settings(QStringLiteral("svt_alice")), tokens, error, 60000));
    QVERIFY2(error.contains(QStringLiteral("cancelled or refused")), qPrintable(error));
}

QTEST_GUILESS_MAIN(VaultSignInTest)
#include "tst_vault_signin.moc"
