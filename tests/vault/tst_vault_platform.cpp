// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// Tenant isolation between the platform (SaaS) tier and organization admins.
// It creates and drops its own database and logins on the server that
// tst_vault_reimport uses, so it needs PGHOST, PGPORT, PGUSER (a superuser on
// a disposable server, for SET SESSION AUTHORIZATION), and PGPASSWORD.

#include "vault/vaultmigrations.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

class VaultPlatformTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void platformOverseesEveryOrganization();
    void organizationAdminsStayInTheirOrganization();
    void serviceAccountIsProtected();
    void suspensionBlocksMembersOnly();

private:
    // Runs `sql` as `login`, returning the first column of the first row.
    // On failure the error text is returned through `error`.
    bool as(const QString &login, const QString &sql, QVariant *value = nullptr, QString *error = nullptr);
    QString role(const QString &name) const { return name + QLatin1Char('_') + m_suffix; }

    QString m_suffix;
    QString m_databaseName;
    QString m_connection = QStringLiteral("vault-platform");
    QString m_acme, m_globex, m_alice, m_bob;
};

namespace {

QSqlDatabase adminDatabase(const QString &name, const QString &database)
{
    auto connection = QSqlDatabase::contains(name) ? QSqlDatabase::database(name, false)
                                                   : QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), name);
    connection.setHostName(qEnvironmentVariable("PGHOST"));
    connection.setPort(qEnvironmentVariableIntValue("PGPORT") > 0 ? qEnvironmentVariableIntValue("PGPORT") : 5432);
    connection.setDatabaseName(database);
    return connection;
}

} // namespace

bool VaultPlatformTest::as(const QString &login, const QString &sql, QVariant *value, QString *error)
{
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.exec(QStringLiteral("RESET SESSION AUTHORIZATION"));
    query.exec(QStringLiteral("SET SESSION AUTHORIZATION %1").arg(login));
    const bool ok = query.exec(sql);
    if (ok && value)
        *value = query.next() ? query.value(0) : QVariant();
    if (!ok && error)
        *error = query.lastError().databaseText();
    query.finish();
    query.exec(QStringLiteral("RESET SESSION AUTHORIZATION"));
    return ok;
}

void VaultPlatformTest::initTestCase()
{
    if (qEnvironmentVariable("SCHEDULEIO_VAULT_TEST_DATABASE").isEmpty())
        QSKIP("Set SCHEDULEIO_VAULT_TEST_DATABASE and the PG* variables to test platform isolation.");
    m_suffix = QString::number(QDateTime::currentMSecsSinceEpoch() % 100000000);
    m_databaseName = QStringLiteral("vault_platform_%1").arg(m_suffix);

    auto server = adminDatabase(QStringLiteral("vault-platform-server"), QStringLiteral("postgres"));
    QVERIFY2(server.open(qEnvironmentVariable("PGUSER"), qEnvironmentVariable("PGPASSWORD")),
             qPrintable(server.lastError().text()));
    QSqlQuery setup(server);
    for (const QString &statement : {
             QStringLiteral("CREATE ROLE %1 NOLOGIN CREATEROLE").arg(role(QStringLiteral("owner"))),
             QStringLiteral("CREATE ROLE %1 LOGIN").arg(role(QStringLiteral("saas"))),
             QStringLiteral("CREATE ROLE %1 LOGIN").arg(role(QStringLiteral("alice"))),
             QStringLiteral("CREATE ROLE %1 LOGIN").arg(role(QStringLiteral("bob"))),
             QStringLiteral("CREATE DATABASE %1 OWNER %2").arg(m_databaseName, role(QStringLiteral("owner")))})
        QVERIFY2(setup.exec(statement), qPrintable(setup.lastError().text()));
    server.close();

    auto database = adminDatabase(m_connection, m_databaseName);
    QVERIFY2(database.open(qEnvironmentVariable("PGUSER"), qEnvironmentVariable("PGPASSWORD")),
             qPrintable(database.lastError().text()));
    QSqlQuery query(database);
    QVERIFY(query.exec(QStringLiteral("SET ROLE %1").arg(role(QStringLiteral("owner")))));
    QStringList applied;
    QString error;
    QVERIFY2(schedule::migrateVault(database, false, applied, error), qPrintable(error));
    QVERIFY2(query.exec(QStringLiteral("SELECT vault.provision_platform_operator('SaaS Admin', '%1')")
                            .arg(role(QStringLiteral("saas")))), qPrintable(query.lastError().text()));
    QVERIFY(query.exec(QStringLiteral("RESET ROLE")));

    m_acme = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_globex = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString saas = role(QStringLiteral("saas"));
    QVariant value;
    QString failure;
    for (const QString &tenant : {m_acme, m_globex}) {
        QVERIFY2(as(saas, QStringLiteral("SELECT vault.platform_create_organization('%1', '%2', 'UTC')")
                              .arg(tenant, tenant == m_acme ? QStringLiteral("Acme") : QStringLiteral("Globex")),
                    nullptr, &failure), qPrintable(failure));
    }
    QVERIFY2(as(saas, QStringLiteral("SELECT vault.manager_enroll_member('%1', 'Alice', '%2', true, true)")
                          .arg(m_acme, role(QStringLiteral("alice"))), &value, &failure), qPrintable(failure));
    m_alice = value.toString();
    QVERIFY2(as(saas, QStringLiteral("SELECT vault.manager_enroll_member('%1', 'Bob', '%2', true, true)")
                          .arg(m_globex, role(QStringLiteral("bob"))), &value, &failure), qPrintable(failure));
    m_bob = value.toString();
    QVERIFY2(as(saas, QStringLiteral("SELECT vault.manager_transfer_ownership('%1', '%2')").arg(m_acme, m_alice),
                nullptr, &failure), qPrintable(failure));
}

void VaultPlatformTest::cleanupTestCase()
{
    if (m_databaseName.isEmpty())
        return;
    QSqlDatabase::database(m_connection).close();
    QSqlDatabase::removeDatabase(m_connection);
    auto server = adminDatabase(QStringLiteral("vault-platform-server"), QStringLiteral("postgres"));
    if (server.open(qEnvironmentVariable("PGUSER"), qEnvironmentVariable("PGPASSWORD"))) {
        QSqlQuery query(server);
        query.exec(QStringLiteral("DROP DATABASE IF EXISTS %1 WITH (FORCE)").arg(m_databaseName));
        for (const QString &name : {QStringLiteral("saas"), QStringLiteral("alice"), QStringLiteral("bob"),
                                    QStringLiteral("carol"), QStringLiteral("owner")})
            query.exec(QStringLiteral("DROP ROLE IF EXISTS %1").arg(role(name)));
        server.close();
    }
    server = QSqlDatabase();
    QSqlDatabase::removeDatabase(QStringLiteral("vault-platform-server"));
}

void VaultPlatformTest::platformOverseesEveryOrganization()
{
    const QString saas = role(QStringLiteral("saas"));
    QVariant value;
    QString error;
    QVERIFY2(as(saas, QStringLiteral("SELECT platform_admin FROM vault.manager_session_profile()"), &value, &error),
             qPrintable(error));
    QVERIFY(value.toBool());
    QVERIFY2(as(saas, QStringLiteral("SELECT count(*) FROM vault.manager_list_organizations()"), &value, &error),
             qPrintable(error));
    QCOMPARE(value.toInt(), 2);
    QVERIFY(as(saas, QStringLiteral("SELECT owner_names FROM vault.platform_list_organizations() "
                                    "WHERE organization_name = 'Acme'"), &value));
    QCOMPARE(value.toString(), QStringLiteral("Alice"));
    // The platform works inside an organization without being a member of it.
    QVERIFY(as(saas, QStringLiteral("SELECT count(*) FROM vault.manager_list_members('%1')").arg(m_globex), &value));
    QCOMPARE(value.toInt(), 1);
    // Its actions are recorded in the organization's audit log, naming the
    // platform administrator behind the service account.
    QSqlQuery audit(QSqlDatabase::database(m_connection));
    QVERIFY(audit.exec(QStringLiteral(
        "SELECT count(*) FROM vault.audit_events AS event JOIN vault.platform_operators AS operator "
        "ON operator.user_id = event.platform_actor_id "
        "WHERE event.organization_id = '%1' AND event.operation = 'organization.ownership_transferred'").arg(m_acme)));
    QVERIFY(audit.next());
    QCOMPARE(audit.value(0).toInt(), 1);
}

void VaultPlatformTest::organizationAdminsStayInTheirOrganization()
{
    const QString alice = role(QStringLiteral("alice"));
    QVariant value;
    QString error;
    QVERIFY(as(alice, QStringLiteral("SELECT platform_admin FROM vault.manager_session_profile()"), &value));
    QVERIFY(!value.toBool());
    QVERIFY(as(alice, QStringLiteral("SELECT count(*) FROM vault.manager_list_organizations()"), &value));
    QCOMPARE(value.toInt(), 1);

    // Another organization's members and the platform are out of reach.
    QVERIFY(!as(alice, QStringLiteral("SELECT * FROM vault.manager_list_members('%1')").arg(m_globex), nullptr, &error));
    QVERIFY(!as(alice, QStringLiteral("SELECT * FROM vault.platform_list_organizations()"), nullptr, &error));
    QVERIFY(!as(alice, QStringLiteral("SELECT vault.platform_set_organization_status('%1', 'suspended')").arg(m_globex),
                nullptr, &error));

    // A login from another organization, or a platform login, can't be added.
    // Both report the same error as a login that doesn't exist.
    QString otherTenant, platformLogin, missing;
    QVERIFY(!as(alice, QStringLiteral("SELECT vault.manager_enroll_member('%1', 'Bob', '%2', true, true)")
                           .arg(m_acme, role(QStringLiteral("bob"))), nullptr, &otherTenant));
    QVERIFY(!as(alice, QStringLiteral("SELECT vault.manager_enroll_member('%1', 'SaaS', '%2', true, true)")
                           .arg(m_acme, role(QStringLiteral("saas"))), nullptr, &platformLogin));
    QVERIFY(!as(alice, QStringLiteral("SELECT vault.manager_enroll_member('%1', 'Nobody', 'no_such_login_%2', true, true)")
                           .arg(m_acme, m_suffix), nullptr, &missing));
    QCOMPARE(otherTenant, missing);
    QCOMPARE(platformLogin, missing);

    // Desktop features can't be turned on for another organization's people.
    QVERIFY(!as(alice, QStringLiteral("SELECT vault.manager_enable_member_capability('%1', '%2', 'time')")
                           .arg(m_acme, m_bob), nullptr, &error));

    // The owner creates and enrolls their own people without the schema owner.
    const QString carol = role(QStringLiteral("carol"));
    QVERIFY2(as(alice, QStringLiteral("SELECT vault.manager_create_login('%1', '%2', "
                                      "'SCRAM-SHA-256$4096:c2FsdHNhbHRzYWx0$aGFzaA==:aGFzaA==')")
                           .arg(m_acme, carol), nullptr, &error), qPrintable(error));
    QVERIFY2(as(alice, QStringLiteral("SELECT vault.manager_enroll_member('%1', 'Carol', '%2', true, true)")
                           .arg(m_acme, carol), &value, &error), qPrintable(error));
    QVERIFY2(as(alice, QStringLiteral("SELECT vault.manager_enable_member_capability('%1', '%2', 'directory')")
                           .arg(m_acme, value.toString()), nullptr, &error), qPrintable(error));
    // A plaintext password is refused, so none reaches the server's logs.
    QVERIFY(!as(alice, QStringLiteral("SELECT vault.manager_create_login('%1', 'plain_%2', 'secret-password')")
                           .arg(m_acme, m_suffix), nullptr, &error));
    QVERIFY2(error.contains(QStringLiteral("SCRAM")), qPrintable(error));
}

void VaultPlatformTest::serviceAccountIsProtected()
{
    const QString alice = role(QStringLiteral("alice"));
    QVariant value;
    QString error;
    // Directories show people only.
    QVERIFY(as(alice, QStringLiteral("SELECT string_agg(display_name, ',' ORDER BY display_name) "
                                     "FROM vault.manager_list_members('%1')").arg(m_acme), &value));
    QVERIFY(!value.toString().contains(QStringLiteral("Service provider")));
    // Logins have no direct table access, and the organization can't change or
    // remove the service account through any path: the trigger holds even
    // for a connection that bypasses row-level security.
    QVERIFY(!as(alice, QStringLiteral("SELECT count(*) FROM vault.organization_service_accounts"), nullptr, &error));
    QVERIFY(!as(alice, QStringLiteral("SELECT vault.manager_set_member_active('%1', "
                                      "(SELECT user_id FROM vault.organization_service_accounts LIMIT 1), false)")
                           .arg(m_acme), nullptr, &error));
    QSqlQuery query(QSqlDatabase::database(m_connection));
    QVERIFY(!query.exec(QStringLiteral(
        "UPDATE vault.organization_members SET active = false "
        "WHERE user_id = (SELECT user_id FROM vault.organization_service_accounts WHERE organization_id = '%1')")
        .arg(m_acme)));
    QVERIFY2(query.lastError().databaseText().contains(QStringLiteral("managed by the platform")),
             qPrintable(query.lastError().databaseText()));
}

void VaultPlatformTest::suspensionBlocksMembersOnly()
{
    const QString saas = role(QStringLiteral("saas"));
    const QString alice = role(QStringLiteral("alice"));
    QVariant value;
    QString error;
    QVERIFY2(as(saas, QStringLiteral("SELECT vault.platform_set_organization_status('%1', 'suspended')").arg(m_acme),
                &value, &error), qPrintable(error));
    QVERIFY(value.toBool());
    QVERIFY(as(alice, QStringLiteral("SELECT count(*) FROM vault.manager_list_organizations()"), &value));
    QCOMPARE(value.toInt(), 0);
    QVERIFY(!as(alice, QStringLiteral("SELECT * FROM vault.manager_list_members('%1')").arg(m_acme), nullptr, &error));
    QVERIFY2(error.contains(QStringLiteral("suspended")), qPrintable(error));
    QVERIFY(as(saas, QStringLiteral("SELECT count(*) FROM vault.manager_list_members('%1')").arg(m_acme), &value));
    QVERIFY(value.toInt() >= 1);

    QVERIFY(as(saas, QStringLiteral("SELECT vault.platform_set_organization_status('%1', 'active')").arg(m_acme)));
    QVERIFY(as(alice, QStringLiteral("SELECT count(*) FROM vault.manager_list_organizations()"), &value));
    QCOMPARE(value.toInt(), 1);
    QVERIFY(as(saas, QStringLiteral("SELECT count(*) FROM vault.platform_list_events(50) "
                                    "WHERE operation LIKE 'platform.organization_%%'"), &value));
    QVERIFY(value.toInt() >= 4);
}

QTEST_GUILESS_MAIN(VaultPlatformTest)
#include "tst_vault_platform.moc"
