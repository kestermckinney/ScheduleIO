// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// The single sign-on credential broker's database side (vault/migrations/009_sign_in.sql):
// which logins vault.broker_issue_sign_in will open, and how.
//
// Needs a PostgreSQL superuser in PGHOST/PGUSER/PGPASSWORD; it creates and
// drops its own database and roles. Skipped without PGHOST.

#include "vault/vaultmigrations.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTest>
#include <QUuid>

namespace {

QString scramVerifier(const QString &password)
{
    constexpr int iterations = 4096;
    QByteArray salt(16, Qt::Uninitialized);
    QRandomGenerator::system()->generate(reinterpret_cast<quint32 *>(salt.data()),
                                         reinterpret_cast<quint32 *>(salt.data() + salt.size()));
    const QByteArray secret = password.toUtf8();
    const auto hmac = [](const QByteArray &key, const QByteArray &message) {
        return QMessageAuthenticationCode::hash(message, key, QCryptographicHash::Sha256);
    };
    QByteArray block = hmac(secret, salt + QByteArray::fromHex("00000001"));
    QByteArray salted = block;
    for (int round = 1; round < iterations; ++round) {
        block = hmac(secret, block);
        for (int i = 0; i < salted.size(); ++i)
            salted[i] = char(salted.at(i) ^ block.at(i));
    }
    const QByteArray storedKey = QCryptographicHash::hash(hmac(salted, "Client Key"), QCryptographicHash::Sha256);
    const QByteArray serverKey = hmac(salted, "Server Key");
    return QStringLiteral("SCRAM-SHA-256$%1:%2$%3:%4")
        .arg(iterations)
        .arg(QString::fromLatin1(salt.toBase64()), QString::fromLatin1(storedKey.toBase64()),
             QString::fromLatin1(serverKey.toBase64()));
}

QString randomPassword()
{
    return QUuid::createUuid().toString(QUuid::Id128) + QUuid::createUuid().toString(QUuid::Id128);
}

QString id(const QUuid &value) { return value.toString(QUuid::WithoutBraces); }

const QString issuer = QStringLiteral("https://example.supabase.co/auth/v1");

} // namespace

class VaultBrokerTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void issuesAnExpiringPasswordForTheMatchingMember();
    void refusesOtherProvidersAndDirectories();
    void refusesSessionsOutsideTheBrokerRole();
    void refusesUnknownPeopleAndBadVerifiers();
    void keepsTheFirstLinkedAccount();
    void refusesALastingPasswordForSingleSignOn();
    void limitsRepeatedSignIns();
    void refusesALastingPasswordBeforeTheFirstSignIn();

private:
    bool exec(QSqlDatabase &database, const QString &statement, QString *error = nullptr);
    QSqlDatabase open(const QString &name, const QString &user, const QString &password);
    // Calls broker_issue_sign_in as the broker; returns the login or "" with error.
    QString issue(const QString &subject, const QString &email, const QString &provider,
                  const QString &directory, const QString &password, QString &error);

    QString m_suffix;
    QString m_database;
    QString m_owner, m_member, m_other, m_broker, m_brokerPassword;
    QUuid m_tenant, m_person;
    bool m_ready = false;
};

bool VaultBrokerTest::exec(QSqlDatabase &database, const QString &statement, QString *error)
{
    QSqlQuery query(database);
    if (query.exec(statement))
        return true;
    if (error)
        *error = query.lastError().text();
    else
        qWarning().noquote() << statement << query.lastError().text();
    return false;
}

QSqlDatabase VaultBrokerTest::open(const QString &name, const QString &user, const QString &password)
{
    auto database = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), name);
    database.setDatabaseName(m_database);
    database.setHostName(qEnvironmentVariable("PGHOST"));
    database.setPort(qEnvironmentVariableIntValue("PGPORT") > 0 ? qEnvironmentVariableIntValue("PGPORT") : 5432);
    database.open(user, password);
    return database;
}

void VaultBrokerTest::initTestCase()
{
    if (qEnvironmentVariable("PGHOST").isEmpty())
        QSKIP("Set PGHOST, PGUSER and PGPASSWORD to a PostgreSQL superuser.");
    m_suffix = QUuid::createUuid().toString(QUuid::Id128).left(8);
    m_database = QStringLiteral("vault_broker_%1").arg(m_suffix);
    m_owner = QStringLiteral("broker_owner_%1").arg(m_suffix);
    m_member = QStringLiteral("broker_member_%1").arg(m_suffix);
    m_other = QStringLiteral("broker_other_%1").arg(m_suffix);
    m_broker = QStringLiteral("broker_service_%1").arg(m_suffix);
    m_brokerPassword = randomPassword();

    {
        auto server = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), QStringLiteral("server"));
        server.setDatabaseName(QStringLiteral("postgres"));
        server.setHostName(qEnvironmentVariable("PGHOST"));
        server.setPort(qEnvironmentVariableIntValue("PGPORT") > 0 ? qEnvironmentVariableIntValue("PGPORT") : 5432);
        QVERIFY2(server.open(qEnvironmentVariable("PGUSER"), qEnvironmentVariable("PGPASSWORD")),
                 qPrintable(server.lastError().text()));
        QVERIFY(exec(server, QStringLiteral("CREATE DATABASE %1").arg(m_database)));
        QVERIFY(exec(server, QStringLiteral("CREATE ROLE %1 NOLOGIN CREATEROLE").arg(m_owner)));
        QVERIFY(exec(server, QStringLiteral("GRANT %1 TO CURRENT_USER WITH SET TRUE").arg(m_owner)));
        QVERIFY(exec(server, QStringLiteral("GRANT CREATE ON DATABASE %1 TO %2").arg(m_database, m_owner)));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("server"));

    auto admin = open(QStringLiteral("admin"), qEnvironmentVariable("PGUSER"), qEnvironmentVariable("PGPASSWORD"));
    QVERIFY2(admin.isOpen(), qPrintable(admin.lastError().text()));
    QVERIFY(exec(admin, QStringLiteral("SET ROLE %1").arg(m_owner)));
    QString error;
    QStringList migrations;
    QVERIFY2(schedule::migrateVault(admin, false, migrations, error), qPrintable(error));

    // The owner creates the logins, so it administers them (PostgreSQL 16+).
    QVERIFY(exec(admin, QStringLiteral("CREATE ROLE %1 LOGIN").arg(m_member)));
    QVERIFY(exec(admin, QStringLiteral("CREATE ROLE %1 LOGIN").arg(m_other)));
    m_tenant = QUuid::createUuid();
    m_person = QUuid::createUuid();
    QVERIFY(exec(admin, QStringLiteral("SELECT vault.provision_organization('%1', 'Acme', 'UTC', '%2', "
                                       "'Ann Example', '%3', true, false)")
                            .arg(id(m_tenant), id(m_person), m_member)));
    QVERIFY(exec(admin, QStringLiteral("GRANT EXECUTE ON FUNCTION vault.manager_sign_in(uuid) TO %1").arg(m_member)));
    QVERIFY(exec(admin, QStringLiteral("CREATE ROLE %1 LOGIN PASSWORD '%2'").arg(m_broker, m_brokerPassword)));
    QVERIFY(exec(admin, QStringLiteral("SELECT vault.add_sign_in_broker('%1')").arg(m_broker)));

    // Acme signs in with Microsoft Entra tenant "acme-tenant"; Ann's sign-in
    // email is ann@acme.example. (Set directly; Manager's path has its own tests.)
    QVERIFY(exec(admin, QStringLiteral("RESET ROLE")));
    QVERIFY(exec(admin, QStringLiteral("INSERT INTO vault.organization_sign_in (organization_id, method, provider, directory) "
                                       "VALUES ('%1', 'sso', 'azure', 'acme-tenant')").arg(id(m_tenant))));
    QVERIFY(exec(admin, QStringLiteral("UPDATE vault.users SET sign_in_email = 'ann@acme.example' WHERE id = '%1'")
                            .arg(id(m_person))));
    m_ready = true;
}

void VaultBrokerTest::cleanupTestCase()
{
    for (const QString &name : QSqlDatabase::connectionNames()) {
        QSqlDatabase::database(name).close();
        QSqlDatabase::removeDatabase(name);
    }
    if (m_suffix.isEmpty())
        return;
    {
        auto server = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), QStringLiteral("cleanup"));
        server.setDatabaseName(QStringLiteral("postgres"));
        server.setHostName(qEnvironmentVariable("PGHOST"));
        server.setPort(qEnvironmentVariableIntValue("PGPORT") > 0 ? qEnvironmentVariableIntValue("PGPORT") : 5432);
        if (server.open(qEnvironmentVariable("PGUSER"), qEnvironmentVariable("PGPASSWORD"))) {
            exec(server, QStringLiteral("DROP DATABASE IF EXISTS %1 WITH (FORCE)").arg(m_database));
            for (const QString &role : {m_broker, m_member, m_other, m_owner})
                exec(server, QStringLiteral("DROP ROLE IF EXISTS %1").arg(role));
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("cleanup"));
}

QString VaultBrokerTest::issue(const QString &subject, const QString &email, const QString &provider,
                               const QString &directory, const QString &password, QString &error)
{
    auto broker = QSqlDatabase::database(QStringLiteral("broker"), false);
    if (!broker.isValid())
        broker = open(QStringLiteral("broker"), m_broker, m_brokerPassword);
    if (!broker.isOpen() && !broker.open(m_broker, m_brokerPassword)) {
        error = broker.lastError().text();
        return {};
    }
    QSqlQuery query(broker);
    query.prepare(QStringLiteral("SELECT login FROM vault.broker_issue_sign_in(?, ?, ?, ?, ?, ?)"));
    query.addBindValue(issuer);
    query.addBindValue(subject);
    query.addBindValue(email);
    query.addBindValue(provider);
    query.addBindValue(directory);
    query.addBindValue(password.isEmpty() ? QStringLiteral("not a verifier") : scramVerifier(password));
    if (!query.exec() || !query.next()) {
        error = query.lastError().text();
        return {};
    }
    return query.value(0).toString();
}

void VaultBrokerTest::issuesAnExpiringPasswordForTheMatchingMember()
{
    QVERIFY(m_ready);
    const QString password = randomPassword();
    QString error;
    QCOMPARE(issue(QStringLiteral("subject-ann"), QStringLiteral("Ann@Acme.example"), QStringLiteral("azure"),
                   QStringLiteral("ACME-TENANT"), password, error),
             m_member);

    // The password opens the login, and the Vault accepts the session.
    {
        auto member = open(QStringLiteral("member"), m_member, password);
        QVERIFY2(member.isOpen(), qPrintable(member.lastError().text()));
        QSqlQuery query(member);
        QVERIFY2(query.exec(QStringLiteral("SELECT vault.manager_sign_in('%1')::text").arg(id(m_tenant))),
                 qPrintable(query.lastError().text()));
        QVERIFY(query.exec(QStringLiteral("SELECT rolvaliduntil - now() FROM pg_roles WHERE rolname = current_user")));
        QVERIFY(query.next());
        const QString remaining = query.value(0).toString();
        QVERIFY2(remaining.startsWith(QStringLiteral("00:04:")) || remaining.startsWith(QStringLiteral("00:05:")),
                 qPrintable(remaining));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("member"));

    // ...and the person is now linked to the account.
    auto admin = QSqlDatabase::database(QStringLiteral("admin"));
    QSqlQuery query(admin);
    QVERIFY(query.exec(QStringLiteral("SELECT issuer, subject FROM vault.users WHERE id = '%1'").arg(id(m_person))));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), issuer);
    QCOMPARE(query.value(1).toString(), QStringLiteral("subject-ann"));
}

void VaultBrokerTest::refusesOtherProvidersAndDirectories()
{
    QVERIFY(m_ready);
    QString error;
    QVERIFY(issue(QStringLiteral("subject-ann"), {}, QStringLiteral("azure"), QStringLiteral("other-tenant"),
                  randomPassword(), error).isEmpty());
    QVERIFY2(error.contains(QStringLiteral("doesn't open a Vault login")), qPrintable(error));
    QVERIFY(issue(QStringLiteral("subject-ann"), {}, QStringLiteral("google"), QStringLiteral("acme-tenant"),
                  randomPassword(), error).isEmpty());
    QVERIFY2(error.contains(QStringLiteral("doesn't open a Vault login")), qPrintable(error));
}

void VaultBrokerTest::refusesSessionsOutsideTheBrokerRole()
{
    QVERIFY(m_ready);
    auto admin = QSqlDatabase::database(QStringLiteral("admin"));
    QVERIFY(exec(admin, QStringLiteral("ALTER ROLE %1 PASSWORD 'other-password'").arg(m_other)));
    QVERIFY(exec(admin, QStringLiteral("GRANT USAGE ON SCHEMA vault TO %1").arg(m_other)));
    QVERIFY(exec(admin, QStringLiteral("GRANT SELECT ON vault.users TO %1").arg(m_other)));
    {
        auto other = open(QStringLiteral("other"), m_other, QStringLiteral("other-password"));
        QVERIFY2(other.isOpen(), qPrintable(other.lastError().text()));
        QSqlQuery query(other);
        QVERIFY(!query.exec(QStringLiteral("SELECT * FROM vault.broker_issue_sign_in('%1', 'subject-ann', NULL, "
                                           "'azure', 'acme-tenant', '%2')").arg(issuer, scramVerifier(QStringLiteral("x")))));
        QVERIFY2(query.lastError().text().contains(QStringLiteral("permission denied")),
                 qPrintable(query.lastError().text()));
        // A session can't turn the broker's read policies on for itself.
        QVERIFY(query.exec(QStringLiteral("SELECT set_config('app.sign_in_broker', 'on', false)")));
        QVERIFY(query.exec(QStringLiteral("SELECT count(*) FROM vault.users")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 0);
    }
    QSqlDatabase::removeDatabase(QStringLiteral("other"));
}

void VaultBrokerTest::refusesUnknownPeopleAndBadVerifiers()
{
    QVERIFY(m_ready);
    QString error;
    QVERIFY(issue(QStringLiteral("subject-zed"), QStringLiteral("zed@acme.example"), QStringLiteral("azure"),
                  QStringLiteral("acme-tenant"), randomPassword(), error).isEmpty());
    QVERIFY2(error.contains(QStringLiteral("doesn't open a Vault login")), qPrintable(error));
    QVERIFY(issue(QStringLiteral("subject-ann"), {}, QStringLiteral("azure"), QStringLiteral("acme-tenant"),
                  QString(), error).isEmpty());
    QVERIFY2(error.contains(QStringLiteral("SCRAM-SHA-256 password verifier")), qPrintable(error));
}

void VaultBrokerTest::keepsTheFirstLinkedAccount()
{
    QVERIFY(m_ready);
    // Ann is linked to subject-ann: another account with her email doesn't match.
    QString error;
    QVERIFY(issue(QStringLiteral("subject-impostor"), QStringLiteral("ann@acme.example"), QStringLiteral("azure"),
                  QStringLiteral("acme-tenant"), randomPassword(), error).isEmpty());
    QVERIFY2(error.contains(QStringLiteral("doesn't open a Vault login")), qPrintable(error));
}

void VaultBrokerTest::refusesALastingPasswordForSingleSignOn()
{
    QVERIFY(m_ready);
    auto admin = QSqlDatabase::database(QStringLiteral("admin"));
    QVERIFY(exec(admin, QStringLiteral("ALTER ROLE %1 PASSWORD 'lasting-password' VALID UNTIL 'infinity'").arg(m_member)));
    {
        auto member = open(QStringLiteral("lasting"), m_member, QStringLiteral("lasting-password"));
        QVERIFY2(member.isOpen(), qPrintable(member.lastError().text()));
        QSqlQuery query(member);
        QVERIFY(!query.exec(QStringLiteral("SELECT vault.manager_sign_in('%1')").arg(id(m_tenant))));
        QVERIFY2(query.lastError().text().contains(QStringLiteral("signs in with single sign-on")),
                 qPrintable(query.lastError().text()));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("lasting"));
    QVERIFY(exec(admin, QStringLiteral("ALTER ROLE %1 PASSWORD NULL").arg(m_member)));
}

void VaultBrokerTest::limitsRepeatedSignIns()
{
    QVERIFY(m_ready);
    QString error;
    int issued = 1;  // the first test's sign-in
    for (; issued < 10; ++issued) {
        if (issue(QStringLiteral("subject-ann"), {}, QStringLiteral("azure"), QStringLiteral("acme-tenant"),
                  randomPassword(), error).isEmpty())
            break;
    }
    QCOMPARE(issued, 6);
    QVERIFY2(error.contains(QStringLiteral("Too many sign-ins")), qPrintable(error));
}

void VaultBrokerTest::refusesALastingPasswordBeforeTheFirstSignIn()
{
    // Bo is in Acme but has never signed in, so isn't linked yet: the
    // organization's method alone refuses his lasting password.
    QVERIFY(m_ready);
    auto admin = QSqlDatabase::database(QStringLiteral("admin"));
    const QString bo = QStringLiteral("broker_bo_%1").arg(m_suffix);
    const QUuid person = QUuid::createUuid();
    QVERIFY(exec(admin, QStringLiteral("SET ROLE %1").arg(m_owner)));
    QVERIFY(exec(admin, QStringLiteral("CREATE ROLE %1 LOGIN PASSWORD 'lasting-password'").arg(bo)));
    QVERIFY(exec(admin, QStringLiteral("GRANT USAGE ON SCHEMA vault TO %1").arg(bo)));
    QVERIFY(exec(admin, QStringLiteral("GRANT EXECUTE ON FUNCTION vault.manager_sign_in(uuid) TO %1").arg(bo)));
    QVERIFY(exec(admin, QStringLiteral("RESET ROLE")));
    QVERIFY(exec(admin, QStringLiteral("INSERT INTO vault.users (id, issuer, subject, display_name, home_organization_id) "
                                       "VALUES ('%1', 'postgresql', '%1', 'Bo Example', '%2')").arg(id(person), id(m_tenant))));
    QVERIFY(exec(admin, QStringLiteral("INSERT INTO vault.organization_members (organization_id, user_id, role) "
                                       "VALUES ('%1', '%2', 'admin')").arg(id(m_tenant), id(person))));
    QVERIFY(exec(admin, QStringLiteral("INSERT INTO vault.manager_principals (database_role_id, database_role_name, user_id) "
                                       "SELECT oid, rolname, '%1' FROM pg_roles WHERE rolname = '%2'").arg(id(person), bo)));
    {
        auto session = open(QStringLiteral("bo"), bo, QStringLiteral("lasting-password"));
        QVERIFY2(session.isOpen(), qPrintable(session.lastError().text()));
        QSqlQuery query(session);
        QVERIFY(!query.exec(QStringLiteral("SELECT vault.manager_sign_in('%1')").arg(id(m_tenant))));
        QVERIFY2(query.lastError().text().contains(QStringLiteral("signs in with single sign-on")),
                 qPrintable(query.lastError().text()));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("bo"));
    QVERIFY(exec(admin, QStringLiteral("DELETE FROM vault.manager_principals WHERE database_role_name = '%1'").arg(bo)));
    QVERIFY(exec(admin, QStringLiteral("DROP OWNED BY %1").arg(bo)));
    QVERIFY(exec(admin, QStringLiteral("DROP ROLE %1").arg(bo)));
}

QTEST_GUILESS_MAIN(VaultBrokerTest)
#include "tst_vault_broker.moc"
