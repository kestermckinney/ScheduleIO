// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "vault/vaultconnection.h"

#include <QSqlDatabase>
#include <QtTest>

using schedule::VaultConnectionSettings;

class VaultConnectionTest : public QObject
{
    Q_OBJECT

private slots:
    void defaultsDescribeALocalVault();
    void localServersSkipCertificateChecks();
    void parsesConnectionUri();
    void rejectsUnusableUri_data();
    void rejectsUnusableUri();
    void identityIgnoresHostCaseAndWhitespace();
    void configuresQpsqlConnection();
    void parsesSingleSignOnUri();
    void configuresSingleSignOnConnection();
};

void VaultConnectionTest::defaultsDescribeALocalVault()
{
    const VaultConnectionSettings settings;
    QCOMPARE(settings.host, QStringLiteral("localhost"));
    QCOMPARE(settings.port, 5432);
    QCOMPARE(settings.database, QStringLiteral("schedule_vault"));
    QVERIFY(settings.user.isEmpty());
    QVERIFY(!settings.isComplete());
}

void VaultConnectionTest::localServersSkipCertificateChecks()
{
    QVERIFY(!schedule::defaultVerifyTls(QStringLiteral("localhost")));
    QVERIFY(!schedule::defaultVerifyTls(QStringLiteral(" 127.0.0.1 ")));
    QVERIFY(!schedule::defaultVerifyTls(QStringLiteral("::1")));
    QVERIFY(schedule::defaultVerifyTls(QStringLiteral("vault.example.com")));
    QCOMPARE(schedule::vaultConnectOptions(true), QStringLiteral("sslmode=verify-full;connect_timeout=5"));
    QCOMPARE(schedule::vaultConnectOptions(false), QStringLiteral("sslmode=prefer;connect_timeout=5"));
}

void VaultConnectionTest::parsesConnectionUri()
{
    VaultConnectionSettings settings;
    QString password, error;
    QVERIFY2(schedule::parseVaultConnectionUri(
                 QStringLiteral("postgresql://pat%40corp:s%3Acret@db.example.com:6543/plans?sslmode=prefer"),
                 settings, password, error), qPrintable(error));
    QCOMPARE(settings.host, QStringLiteral("db.example.com"));
    QCOMPARE(settings.port, 6543);
    QCOMPARE(settings.database, QStringLiteral("plans"));
    QCOMPARE(settings.user, QStringLiteral("pat@corp"));
    QCOMPARE(password, QStringLiteral("s:cret"));
    QVERIFY(!settings.verifyTls);

    QVERIFY(schedule::parseVaultConnectionUri(QStringLiteral("postgres://pat@db.example.com/plans"),
                                              settings, password, error));
    QCOMPARE(settings.port, VaultConnectionSettings::DefaultPort);
    QVERIFY(settings.verifyTls);
    QVERIFY(password.isEmpty());
}

void VaultConnectionTest::rejectsUnusableUri_data()
{
    QTest::addColumn<QString>("uri");
    QTest::newRow("not a uri") << QStringLiteral("localhost");
    QTest::newRow("other scheme") << QStringLiteral("mysql://pat@db/plans");
    QTest::newRow("no user") << QStringLiteral("postgresql://db/plans");
    QTest::newRow("no database") << QStringLiteral("postgresql://pat@db/");
    QTest::newRow("two hosts") << QStringLiteral("postgresql://pat@a,b/plans");
    QTest::newRow("unsupported sslmode") << QStringLiteral("postgresql://pat@db/plans?sslmode=disable");
    QTest::newRow("other option") << QStringLiteral("postgresql://pat@db/plans?application_name=x");
    QTest::newRow("issuer not a web address") << QStringLiteral("postgresql://pat@db/plans?oauth_issuer=keycloak");
    QTest::newRow("client without issuer") << QStringLiteral("postgresql://pat@db/plans?oauth_client_id=app");
    QTest::newRow("provider without issuer") << QStringLiteral("postgresql://pat@db/plans?idp=acme");
    QTest::newRow("issuer twice")
        << QStringLiteral("postgresql://pat@db/plans?oauth_issuer=https://a/realms/v&oauth_issuer=https://b/realms/v");
}

void VaultConnectionTest::rejectsUnusableUri()
{
    QFETCH(QString, uri);
    VaultConnectionSettings settings;
    settings.user = QStringLiteral("unchanged");
    QString password = QStringLiteral("unchanged"), error;
    QVERIFY(!schedule::parseVaultConnectionUri(uri, settings, password, error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(settings.user, QStringLiteral("unchanged"));
    QCOMPARE(password, QStringLiteral("unchanged"));
}

void VaultConnectionTest::identityIgnoresHostCaseAndWhitespace()
{
    VaultConnectionSettings a, b;
    a.user = b.user = QStringLiteral("pat");
    a.host = QStringLiteral("DB.example.com ");
    b.host = QStringLiteral("db.example.com");
    QCOMPARE(a.identity(), b.identity());
    b.database = QStringLiteral("other");
    QVERIFY(a.identity() != b.identity());
}

void VaultConnectionTest::configuresQpsqlConnection()
{
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QPSQL")))
        QSKIP("The Qt PostgreSQL driver isn't installed.");
    VaultConnectionSettings settings;
    settings.host = QStringLiteral(" db.example.com ");
    settings.user = QStringLiteral("pat");
    settings.verifyTls = true;
    const QString name = QStringLiteral("tst_vaultconnection");
    {
        const QSqlDatabase database = schedule::addVaultDatabase(name, settings);
        QCOMPARE(database.hostName(), QStringLiteral("db.example.com"));
        QCOMPARE(database.port(), 5432);
        QCOMPARE(database.databaseName(), QStringLiteral("schedule_vault"));
        QCOMPARE(database.userName(), QStringLiteral("pat"));
        QCOMPARE(database.connectOptions(), schedule::vaultConnectOptions(true));
    }
    QSqlDatabase::removeDatabase(name);
}

void VaultConnectionTest::parsesSingleSignOnUri()
{
    VaultConnectionSettings settings;
    QString password, error;
    QVERIFY2(schedule::parseVaultConnectionUri(
                 QStringLiteral("postgresql://pat@db.example.com/plans?sslmode=verify-full"
                                "&oauth_issuer=https%3A%2F%2Fsign-in.example.com%2Frealms%2Fvault&idp=acme"),
                 settings, password, error), qPrintable(error));
    QVERIFY(settings.usesSingleSignOn());
    QCOMPARE(settings.signInIssuer, QStringLiteral("https://sign-in.example.com/realms/vault"));
    QCOMPARE(settings.signInClientId, VaultConnectionSettings::defaultSignInClientId());
    QCOMPARE(settings.signInProvider, QStringLiteral("acme"));
    QVERIFY(password.isEmpty());

    QVERIFY(schedule::parseVaultConnectionUri(QStringLiteral("postgresql://pat@db/plans"), settings, password, error));
    QVERIFY(!settings.usesSingleSignOn());
    QVERIFY(settings.signInClientId.isEmpty());
}

void VaultConnectionTest::configuresSingleSignOnConnection()
{
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QPSQL")))
        QSKIP("The Qt PostgreSQL driver isn't installed.");
    VaultConnectionSettings settings;
    settings.user = QStringLiteral("pat");
    settings.signInIssuer = QStringLiteral("https://sign-in.example.com/realms/vault");
    const QString name = QStringLiteral("tst_vaultconnection_sso");
    {
        const QSqlDatabase database = schedule::addVaultDatabase(name, settings);
        QCOMPARE(database.connectOptions(),
                 schedule::vaultConnectOptions(settings.verifyTls)
                     + QStringLiteral(";oauth_issuer='https://sign-in.example.com/realms/vault'"
                                      ";oauth_client_id='schedulevault'"));
    }
    QSqlDatabase::removeDatabase(name);
}

QTEST_GUILESS_MAIN(VaultConnectionTest)
#include "tst_vaultconnection.moc"
