// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#include "vaultmigrations.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace schedule {
namespace {

bool execute(QSqlQuery &query, const QString &sql, QString &error)
{
    if (query.exec(sql))
        return true;
    error = query.lastError().text();
    return false;
}

// Read definitions instead of object OIDs, so restoring the same Vault into
// another database on the same PostgreSQL major version keeps the fingerprint.
bool catalogDigest(QSqlQuery &query, QString &digest, QString &error)
{
    const auto sql = QStringLiteral(R"SQL(
        WITH relations AS (
            SELECT relation.* FROM pg_class AS relation
            JOIN pg_namespace AS schema ON schema.oid = relation.relnamespace
            WHERE schema.nspname = 'vault'
        ), definitions AS (
            SELECT jsonb_build_array('relation', relname, relkind, relpersistence,
                       relrowsecurity, relforcerowsecurity, relreplident, pg_get_userbyid(relowner)) AS item
            FROM relations
            UNION ALL
            SELECT jsonb_build_array('column', relation.relname, column_info.attnum, column_info.attname,
                       format_type(column_info.atttypid, column_info.atttypmod), column_info.attnotnull,
                       column_info.attidentity, column_info.attgenerated, pg_get_expr(default_value.adbin, relation.oid))
            FROM relations AS relation
            JOIN pg_attribute AS column_info ON column_info.attrelid = relation.oid
            LEFT JOIN pg_attrdef AS default_value
              ON default_value.adrelid = relation.oid AND default_value.adnum = column_info.attnum
            WHERE relation.relkind IN ('r','p','v','m','f')
              AND column_info.attnum > 0 AND NOT column_info.attisdropped
            UNION ALL
            SELECT jsonb_build_array('constraint', relation.relname, rule.conname,
                       rule.convalidated, pg_get_constraintdef(rule.oid))
            FROM relations AS relation JOIN pg_constraint AS rule ON rule.conrelid = relation.oid
            UNION ALL
            SELECT jsonb_build_array('index', relname, pg_get_indexdef(oid))
            FROM relations WHERE relkind IN ('i','I')
            UNION ALL
            SELECT jsonb_build_array('view', relname, pg_get_viewdef(oid))
            FROM relations WHERE relkind IN ('v','m')
            UNION ALL
            SELECT jsonb_build_array('policy', tablename, policyname, permissive,
                       (SELECT array_agg(role_name ORDER BY role_name) FROM unnest(roles) AS role_name),
                       cmd, qual, with_check)
            FROM pg_policies WHERE schemaname = 'vault'
            UNION ALL
            SELECT jsonb_build_array('trigger', relation.relname, trigger.tgname,
                       trigger.tgenabled, pg_get_triggerdef(trigger.oid))
            FROM relations AS relation JOIN pg_trigger AS trigger ON trigger.tgrelid = relation.oid
            WHERE NOT trigger.tgisinternal
            UNION ALL
            SELECT jsonb_build_array('routine', pg_get_userbyid(routine.proowner),
                       pg_get_functiondef(routine.oid))
            FROM pg_proc AS routine JOIN pg_namespace AS schema ON schema.oid = routine.pronamespace
            WHERE schema.nspname = 'vault' AND routine.prokind IN ('f','p','w')
        )
        SELECT coalesce(jsonb_agg(item ORDER BY item::text COLLATE "C"), '[]'::jsonb)::text
        FROM definitions
    )SQL");
    if (!execute(query, sql, error))
        return false;
    if (!query.next()) {
        error = query.lastError().isValid() ? query.lastError().text()
                                           : QStringLiteral("Could not inspect the Vault catalog");
        return false;
    }
    digest = QString::fromLatin1(
        QCryptographicHash::hash(query.value(0).toString().toUtf8(), QCryptographicHash::Sha256).toHex());
    query.finish();
    return true;
}

bool validateMigrationOwner(QSqlQuery &query, QString &error)
{
    if (!execute(query, QStringLiteral(
            "SELECT role.rolsuper OR role.rolbypassrls, role.rolname::text, EXISTS ("
            "SELECT FROM pg_catalog.pg_namespace AS schema "
            "WHERE schema.nspname IN ('vault', 'vault_admin') AND schema.nspowner <> role.oid"
            ") FROM pg_catalog.pg_roles AS role WHERE role.rolname = current_user"), error))
        return false;
    if (!query.next()) {
        error = query.lastError().isValid() ? query.lastError().text()
                                           : QStringLiteral("Could not identify the PostgreSQL role");
        return false;
    }
    if (query.value(0).toBool()) {
        error = QStringLiteral("The effective PostgreSQL role “%1” has superuser or BYPASSRLS privileges. "
                               "Connect as the Vault schema owner without superuser or BYPASSRLS privileges, "
                               "or enter that owner in Schema → Assume schema owner role.")
                    .arg(query.value(1).toString());
        return false;
    }
    if (query.value(2).toBool()) {
        error = QStringLiteral("The effective PostgreSQL role “%1” does not own the existing Vault schemas. "
                               "Enter their owner in Schema → Assume schema owner role.")
                    .arg(query.value(1).toString());
        return false;
    }
    query.finish();
    return true;
}

bool runMigrations(const QSqlDatabase &database, bool preview, QStringList &pending, QString &error)
{
    QSqlQuery query(database);
    if (!execute(query, QStringLiteral("SET LOCAL search_path = pg_catalog"), error)
        || !execute(query, QStringLiteral("SELECT pg_advisory_xact_lock(173194, 1)"), error)
        || !validateMigrationOwner(query, error)
        || !execute(query, QStringLiteral("CREATE SCHEMA IF NOT EXISTS vault_admin"), error)
        || !execute(query, QStringLiteral(
            "CREATE TABLE IF NOT EXISTS vault_admin.schema_migrations ("
            "name text PRIMARY KEY, sha256 text NOT NULL, "
            "applied_at timestamptz NOT NULL DEFAULT clock_timestamp())"), error)
        || !execute(query, QStringLiteral("SELECT name, sha256 FROM vault_admin.schema_migrations"), error))
        return false;

    QMap<QString, QString> installed;
    while (query.next())
        installed.insert(query.value(0).toString(), query.value(1).toString());
    if (query.lastError().isValid()) {
        error = query.lastError().text();
        return false;
    }
    query.finish();

    const QDir scripts(QStringLiteral(":/scheduleio/vault/migrations"));
    const auto names = scripts.entryList({QStringLiteral("*.sql")}, QDir::Files, QDir::Name);
    if (names.isEmpty()) {
        error = QStringLiteral("Vault migration scripts are missing");
        return false;
    }

    QMap<QString, QByteArray> migrations;
    QMap<QString, QString> digests;
    for (const QString &name : names) {
        QFile file(scripts.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            error = file.errorString();
            return false;
        }
        const QByteArray sql = file.readAll();
        const QString digest = QString::fromLatin1(
            QCryptographicHash::hash(sql, QCryptographicHash::Sha256).toHex());
        digests.insert(name, digest);
        if (!installed.contains(name)) {
            migrations.insert(name, sql);
            pending.append(name);
        } else if (!pending.isEmpty() || installed.take(name) != digest) {
            error = QStringLiteral("Vault migration history diverges at %1").arg(name);
            return false;
        }
    }
    if (!installed.isEmpty()) {
        error = QStringLiteral("Vault contains unknown migration %1").arg(installed.firstKey());
        return false;
    }

    if (!execute(query, QStringLiteral(
            "CREATE TABLE IF NOT EXISTS vault_admin.schema_fingerprint ("
            "singleton boolean PRIMARY KEY DEFAULT true CHECK (singleton), sha256 text NOT NULL)"), error)
        || !execute(query, QStringLiteral("SELECT sha256 FROM vault_admin.schema_fingerprint"), error))
        return false;

    const QString previous = query.next() ? query.value(0).toString() : QString();
    if (query.lastError().isValid()) {
        error = query.lastError().text();
        return false;
    }
    query.finish();

    if (!previous.isEmpty()) {
        QString current;
        if (!catalogDigest(query, current, error))
            return false;
        if (current != previous) {
            error = QStringLiteral("The live Vault schema differs from its recorded migration state");
            return false;
        }
    }

    if (preview)
        return true;

    for (auto it = migrations.cbegin(); it != migrations.cend(); ++it) {
        if (!execute(query, QString::fromUtf8(it.value()), error)) {
            error = QStringLiteral("%1: %2").arg(it.key(), error);
            return false;
        }
        if (!query.prepare(QStringLiteral(
            "INSERT INTO vault_admin.schema_migrations (name, sha256) VALUES (?, ?)"))) {
            error = query.lastError().text();
            return false;
        }
        query.addBindValue(it.key());
        query.addBindValue(digests.value(it.key()));
        if (!query.exec()) {
            error = query.lastError().text();
            return false;
        }
    }
    if (!migrations.isEmpty() || previous.isEmpty()) {
        QString current;
        if (!catalogDigest(query, current, error))
            return false;
        if (!query.prepare(QStringLiteral(
                "INSERT INTO vault_admin.schema_fingerprint (singleton, sha256) VALUES (true, ?) "
                "ON CONFLICT (singleton) DO UPDATE SET sha256 = EXCLUDED.sha256"))) {
            error = query.lastError().text();
            return false;
        }
        query.addBindValue(current);
        if (!query.exec()) {
            error = query.lastError().text();
            return false;
        }
    }
    return true;
}

} // namespace

bool migrateVault(const QSqlDatabase &database, bool preview, QStringList &pending, QString &error)
{
    error.clear();
    if (!database.isOpen() || database.driverName() != QStringLiteral("QPSQL")) {
        error = QStringLiteral("An open PostgreSQL connection is required");
        return false;
    }

    QSqlDatabase transaction = database;
    if (!transaction.transaction()) {
        error = transaction.lastError().text();
        return false;
    }
    QStringList plan;
    if (!runMigrations(database, preview, plan, error)) {
        transaction.rollback();
        return false;
    }
    if (!(preview ? transaction.rollback() : transaction.commit())) {
        error = transaction.lastError().text();
        transaction.rollback();
        return false;
    }
    pending = plan;
    return true;
}

} // namespace schedule
