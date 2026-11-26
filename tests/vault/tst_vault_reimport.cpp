// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "vault/exportselection.h"
#include "vault/vaultmigrations.h"
#include "vault/vaultreader.h"
#include "xmlio.h"
#include "mppio.h"

#include <QFile>

#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QtTest>

namespace {

QString id(const QUuid &uuid)
{
    return uuid.toString(QUuid::WithoutBraces);
}

QByteArray mspdi(const schedule::Project &project)
{
    XmlIO writer;
    writer.setProject(project);
    return writer.saveToData();
}

void verifyMppVaultRoundTrip(const QSqlDatabase &database, const QUuid &tenant)
{
    QFile fixture(QStringLiteral(SCHEDULEIO_FIXTURE_DIR "/Timeline Test.mpp"));
    QVERIFY2(fixture.open(QIODevice::ReadOnly), qPrintable(fixture.errorString()));
    const QByteArray bytes = fixture.readAll();
    MppIO source;
    QVERIFY2(source.openFromData(bytes), qPrintable(source.errorString()));
    QVERIFY(!source.project().mppSourceTemplate.isEmpty());
    QVERIFY(!source.project().timelineView.rawXml.isEmpty());

    const QUuid projectId = QUuid::createUuid();
    QString error;
    QVERIFY2(schedule::importInitialVaultScheduleAsManager(database, tenant, projectId, bytes,
             schedule::VaultSourceFormat::Mpp, {}, {}, error), qPrintable(error));

    schedule::Project restored;
    qint64 counter = -1;
    QVERIFY2(schedule::loadDesktopVaultSchedule(database, tenant, projectId, restored, counter, error),
             qPrintable(error));
    QCOMPARE(counter, qint64(0));
    QVERIFY(restored == source.project());
    QCOMPARE(restored.mppSourceTemplate, source.project().mppSourceTemplate);
    QCOMPARE(restored.mppFontBases, source.project().mppFontBases);
    QCOMPARE(restored.mppBarExceptions, source.project().mppBarExceptions);
    QCOMPARE(restored.timelineView.rawXml, source.project().timelineView.rawXml);
    QVERIFY(restored.timelineView == source.project().timelineView);

    MppIO writer;
    writer.setProject(restored);
    const QByteArray exported = writer.saveToData();
    QVERIFY2(!exported.isEmpty(), qPrintable(writer.errorString()));
    MppIO reopened;
    QVERIFY2(reopened.openFromData(exported), qPrintable(reopened.errorString()));
    QVERIFY(reopened.project().timelineView == source.project().timelineView);
    QCOMPARE(reopened.project().tasks.size(), source.project().tasks.size());
}

} // namespace

class VaultReimportTest : public QObject
{
    Q_OBJECT

private slots:
    void filtersExportReferences();
    void omitsOptionalExportFields();
    void replacesImportedActualsWithoutDoubleCounting();
    void preservesTimeWhenAssignmentMoves();
};

void VaultReimportTest::filtersExportReferences()
{
    schedule::Project source;
    source.mppSourceTemplate = QByteArrayLiteral("opaque");
    schedule::Task task;
    task.uniqueId = 10;
    task.outlineLevel = 1;
    task.summary = true;
    source.tasks.append(task);
    task.summary = false;
    task.outlineLevel = 2;
    task.uniqueId = 11;
    source.tasks.append(task);
    task.uniqueId = 12;
    source.tasks.append(task);

    schedule::Resource resource;
    resource.uniqueId = 20;
    source.resources.append(resource);
    resource.uniqueId = 21;
    source.resources.append(resource);
    schedule::Assignment assignment;
    assignment.uniqueId = 30;
    assignment.taskUniqueId = 11;
    assignment.resourceUniqueId = 20;
    source.assignments.append(assignment);
    assignment.uniqueId = 31;
    assignment.taskUniqueId = 12;
    assignment.resourceUniqueId = 21;
    source.assignments.append(assignment);
    schedule::Relation relation;
    relation.predecessorTaskUid = 11;
    relation.successorTaskUid = 12;
    source.relations.append(relation);

    schedule::Project filtered;
    QString error;
    QVERIFY2(schedule::selectVaultExportTasks(source, {11}, filtered, error), qPrintable(error));
    QCOMPARE(filtered.tasks.size(), 2);
    QCOMPARE(filtered.tasks.first().uniqueId, 10);
    QCOMPARE(filtered.tasks.last().uniqueId, 11);
    QCOMPARE(filtered.assignments.size(), 1);
    QCOMPARE(filtered.assignments.first().uniqueId, 30);
    QCOMPARE(filtered.resources.size(), 1);
    QCOMPARE(filtered.resources.first().uniqueId, 20);
    QVERIFY(filtered.relations.isEmpty());
    QVERIFY(filtered.mppSourceTemplate.isEmpty());

    XmlIO reader;
    QVERIFY2(reader.openFromData(mspdi(filtered)), qPrintable(reader.errorString()));
    QCOMPARE(reader.project().tasks.size(), 2);
    QCOMPARE(reader.project().assignments.first().taskUniqueId, 11);

    QVERIFY2(schedule::selectVaultExportTasks(source, {10}, filtered, error), qPrintable(error));
    QCOMPARE(filtered.tasks.size(), 3);
    QCOMPARE(filtered.relations.size(), 1);
    QVERIFY(!schedule::selectVaultExportTasks(source, {99}, filtered, error));
    QCOMPARE(source.tasks.size(), 3);
}

void VaultReimportTest::omitsOptionalExportFields()
{
    schedule::Project exported;
    exported.calendarUniqueId = 25;
    schedule::Calendar calendar;
    calendar.uniqueId = 25;
    exported.calendars.append(calendar);

    schedule::Task task;
    task.uniqueId = 10;
    task.calendarUniqueId = 25;
    task.baselines.append(schedule::Baseline{});
    task.customFields.append(schedule::CustomField{});
    exported.tasks.append(task);

    schedule::Resource resource;
    resource.uniqueId = 20;
    resource.calendarUniqueId = 25;
    exported.resources.append(resource);

    schedule::Assignment assignment;
    assignment.taskUniqueId = 10;
    assignment.resourceUniqueId = 20;
    schedule::TimephasedValue work;
    work.type = schedule::TimephasedValue::BaselineWork;
    assignment.timephasedValues.append(work);
    work.type = schedule::TimephasedValue::ActualWork;
    assignment.timephasedValues.append(work);
    exported.assignments.append(assignment);
    exported.customFieldDefinitions.append(schedule::CustomField{});

    schedule::applyVaultExportFields(exported, {false, false, false});
    QVERIFY(exported.tasks.first().baselines.isEmpty());
    QVERIFY(exported.tasks.first().customFields.isEmpty());
    QVERIFY(exported.customFieldDefinitions.isEmpty());
    QVERIFY(exported.calendars.isEmpty());
    QCOMPARE(exported.calendarUniqueId, -1);
    QCOMPARE(exported.tasks.first().calendarUniqueId, -1);
    QCOMPARE(exported.resources.first().calendarUniqueId, -1);
    QCOMPARE(exported.assignments.first().timephasedValues.size(), 1);
    QCOMPARE(exported.assignments.first().timephasedValues.first().type,
             int(schedule::TimephasedValue::ActualWork));
}

void VaultReimportTest::replacesImportedActualsWithoutDoubleCounting()
{
    schedule::Project exported;
    schedule::Task task;
    task.uniqueId = 10;
    task.id = 1;
    task.name = QStringLiteral("Exported task");
    exported.tasks.append(task);
    schedule::Resource resource;
    resource.uniqueId = 20;
    resource.id = 1;
    resource.name = QStringLiteral("Planner");
    exported.resources.append(resource);
    schedule::Assignment assignment;
    assignment.uniqueId = 30;
    assignment.taskUniqueId = 10;
    assignment.resourceUniqueId = 20;
    assignment.actualWorkMillis = 7200000;
    assignment.remainingWorkMillis = 3600000;
    assignment.workMillis = 10800000;
    assignment.actualOvertimeWorkMillis = 1000;
    exported.assignments.append(assignment);
    assignment.uniqueId = 31;
    exported.assignments.append(assignment);

    const auto from = QDateTime::fromString(QStringLiteral("2025-01-01T00:00:00Z"), Qt::ISODate);
    const auto summary = schedule::applyVaultApprovedWork(exported,
        {{30, from, from.addDays(1), 1800123, 2}, {-1, from, from.addDays(1), 600000, 1}});
    QCOMPARE(summary.replacedAssignments, 1);
    QCOMPARE(summary.omittedEntries, qint64(1));
    QCOMPARE(exported.assignments.first().actualWorkMillis, qint64(1800123));
    QCOMPARE(exported.assignments.first().remainingWorkMillis, qint64(3600000));
    QCOMPARE(exported.assignments.first().actualOvertimeWorkMillis, qint64(0));
    QCOMPARE(exported.assignments.last().actualWorkMillis, qint64(7200000));

    XmlIO reader;
    QVERIFY2(reader.openFromData(mspdi(exported)), qPrintable(reader.errorString()));
    const auto restored = reader.project().assignments.first();
    QCOMPARE(restored.uniqueId, 30);
    QCOMPARE(restored.timephasedValues.size(), 1);
    const auto bucket = restored.timephasedValues.first();
    QCOMPARE(bucket.type, int(schedule::TimephasedValue::ActualWork));
    QCOMPARE(bucket.start.date(), from.date());
    QCOMPARE(bucket.start.time(), QTime(0, 0));
    QCOMPARE(bucket.finish.date(), from.date().addDays(1));
    QCOMPARE(bucket.finish.time(), QTime(0, 0));
    QCOMPARE(bucket.durationMillis(), qint64(1800123));
}

void VaultReimportTest::preservesTimeWhenAssignmentMoves()
{
    const QString databaseName = qEnvironmentVariable("SCHEDULEIO_VAULT_TEST_DATABASE");
    if (databaseName.isEmpty())
        QSKIP("Set SCHEDULEIO_VAULT_TEST_DATABASE to a fresh, disposable PostgreSQL database.");

    const QString ownerRole = qEnvironmentVariable("SCHEDULEIO_VAULT_TEST_OWNER");
    const QString actorRole = qEnvironmentVariable("SCHEDULEIO_VAULT_TEST_ACTOR");
    const QRegularExpression rolePattern(QStringLiteral("^[a-z_][a-z_0-9]*$"));
    QVERIFY(rolePattern.match(ownerRole).hasMatch());
    QVERIFY(rolePattern.match(actorRole).hasMatch());

    auto database = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"));
    database.setDatabaseName(databaseName);
    database.setHostName(qEnvironmentVariable("PGHOST"));
    database.setPort(qEnvironmentVariableIntValue("PGPORT") > 0 ? qEnvironmentVariableIntValue("PGPORT") : 5432);
    QVERIFY2(database.open(qEnvironmentVariable("PGUSER"), qEnvironmentVariable("PGPASSWORD")),
             qPrintable(database.lastError().text()));

    QSqlQuery sql(database);
    const auto execute = [&](const QString &statement) {
        const bool ok = sql.exec(statement);
        if (!ok)
            qWarning().noquote() << sql.lastError().text();
        return ok;
    };

    QVERIFY(execute(QStringLiteral("SELECT to_regnamespace('vault') IS NULL")));
    QVERIFY(sql.next() && sql.value(0).toBool());
    QString error;
    QStringList migrations;
    QVERIFY(!schedule::migrateVault(database, false, migrations, error));
    QVERIFY(error.contains(QStringLiteral("without superuser or BYPASSRLS")));
    QVERIFY(error.contains(QStringLiteral("Assume schema owner role")));
    QVERIFY(execute(QStringLiteral("SELECT to_regnamespace('vault_admin') IS NULL")));
    QVERIFY(sql.next() && sql.value(0).toBool());
    QVERIFY(execute(QStringLiteral("SET ROLE %1").arg(ownerRole)));
    QVERIFY2(schedule::migrateVault(database, false, migrations, error), qPrintable(error));
    QVERIFY(execute(QStringLiteral("SET ROLE %1").arg(actorRole)));
    QVERIFY(!schedule::migrateVault(database, true, migrations, error));
    QVERIFY(error.contains(actorRole));
    QVERIFY(error.contains(QStringLiteral("does not own the existing Vault schemas")));
    QVERIFY(execute(QStringLiteral("SET ROLE %1").arg(ownerRole)));

    const QUuid tenant = QUuid::createUuid(), actor = QUuid::createUuid();
    const QUuid project = QUuid::createUuid(), entry = QUuid::createUuid();
    QVERIFY(execute(QStringLiteral(
        "SELECT vault.provision_organization('%1', 'Test tenant', 'UTC', '%2', "
        "'Test operator', '%3', true, false)").arg(id(tenant), id(actor), actorRole)));
    QVERIFY(execute(QStringLiteral(
        "GRANT EXECUTE ON FUNCTION vault.desktop_create_time(uuid,uuid,uuid,uuid,uuid,date,bigint,text,boolean), "
        "vault.desktop_list_time(uuid,uuid), "
        "vault.desktop_update_time(uuid,uuid,uuid,text,text,date,bigint,text,boolean), "
        "vault.manager_check_project_access(uuid,uuid,uuid) TO %1").arg(actorRole)));
    QVERIFY(execute(QStringLiteral("RESET ROLE")));
    QVERIFY(execute(QStringLiteral("SET SESSION AUTHORIZATION %1").arg(actorRole)));

    schedule::Project original;
    original.title = QStringLiteral("Original schedule");
    schedule::Task task;
    task.id = 1;
    task.uniqueId = 10;
    task.name = QStringLiteral("First task");
    task.start = QDateTime::fromString(QStringLiteral("2026-11-25T08:00:00Z"), Qt::ISODate);
    task.finish = QDateTime::fromString(QStringLiteral("2026-11-25T12:00:00Z"), Qt::ISODate);
    original.tasks.append(task);
    task.id = 2;
    task.uniqueId = 11;
    task.name = QStringLiteral("Replacement task");
    original.tasks.append(task);
    schedule::Relation relation;
    relation.uniqueId = 1;
    relation.predecessorTaskUid = 10;
    relation.successorTaskUid = 11;
    relation.lagMillis = 3600000;
    original.relations.append(relation);
    schedule::Resource resource;
    resource.id = 1;
    resource.uniqueId = 20;
    resource.name = QStringLiteral("Planner");
    original.resources.append(resource);
    schedule::Assignment assignment;
    assignment.uniqueId = 30;
    assignment.taskUniqueId = 10;
    assignment.resourceUniqueId = 20;
    assignment.workMillis = 7200000;
    original.assignments.append(assignment);

    const QByteArray initialFile = mspdi(original);
    QVERIFY2(schedule::importInitialVaultScheduleAsManager(database, tenant, project, initialFile,
             schedule::VaultSourceFormat::Mspdi, {}, {}, error), qPrintable(error));
    const QString scope = QStringLiteral("'%1','%2'").arg(id(tenant), id(project));
    QVERIFY(!sql.exec(QStringLiteral("UPDATE vault.projects SET title = 'Unauthorized' "
                                     "WHERE (organization_id, id) = (%1)").arg(scope)));
    QCOMPARE(sql.lastError().nativeErrorCode(), QStringLiteral("42501"));

    qint64 deniedCounter = -1;
    QVERIFY(!schedule::reimportDesktopVaultSchedule(database, tenant, project, original, 0, initialFile,
             QUuid::createUuid(), schedule::VaultSourceFormat::Mspdi, {}, {}, {}, deniedCounter, error));
    QVERIFY2(error.contains(QStringLiteral("permission"), Qt::CaseInsensitive), qPrintable(error));

    QVERIFY(execute(QStringLiteral("RESET SESSION AUTHORIZATION")));
    QVERIFY(execute(QStringLiteral("SET ROLE %1").arg(ownerRole)));
    QVERIFY(execute(QStringLiteral("SELECT vault.enable_desktop_capability('%1','schedule_updates')").arg(actorRole)));
    QVERIFY(execute(QStringLiteral("RESET ROLE")));
    QVERIFY(execute(QStringLiteral("SET SESSION AUTHORIZATION %1").arg(actorRole)));
    QVERIFY(execute(QStringLiteral(
        "SELECT record_kind, record_id FROM vault.desktop_read_schedule(%1) "
        "WHERE (record_kind = 'task' AND source_uid = 10) OR record_kind = 'assignment' "
        "ORDER BY record_kind").arg(scope)));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("assignment"));
    const QUuid oldAssignment = sql.value(1).toUuid();
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("task"));
    const QUuid oldTask = sql.value(1).toUuid();

    QVERIFY(execute(QStringLiteral(
        "SELECT vault.desktop_create_time(%1, '%2', '%3', '%4', CURRENT_DATE, 3600000, '', false)")
        .arg(scope, id(entry), id(oldTask), id(oldAssignment))));

    auto revised = original;
    revised.title = QStringLiteral("Revised schedule");
    revised.tasks.front().name = QStringLiteral("Renamed task");
    revised.assignments.front().taskUniqueId = 11;
    const QByteArray revisedFile = mspdi(revised);
    schedule::Project live, incoming;
    qint64 counter = -1;
    QVERIFY2(schedule::previewDesktopVaultReimport(database, tenant, project, revisedFile,
             schedule::VaultSourceFormat::Mspdi, live, incoming, counter, error), qPrintable(error));
    QCOMPARE(counter, qint64(0));

    auto competing = original;
    competing.title = QStringLiteral("Competing schedule update");
    const QByteArray competingFile = mspdi(competing);
    schedule::Project competingLive, competingIncoming;
    qint64 competingCounter = -1;
    QVERIFY2(schedule::previewDesktopVaultReimport(database, tenant, project, competingFile,
             schedule::VaultSourceFormat::Mspdi, competingLive, competingIncoming, competingCounter, error),
             qPrintable(error));
    QCOMPARE(competingCounter, counter);

    const QUuid request = QUuid::createUuid();
    for (int attempt = 0; attempt < 2; ++attempt) {
        QVERIFY2(schedule::reimportDesktopVaultSchedule(database, tenant, project, incoming, 0,
                 revisedFile, request, schedule::VaultSourceFormat::Mspdi, {}, {}, {}, counter, error),
                 qPrintable(error));
        QCOMPARE(counter, qint64(1));
    }

    QVERIFY(!schedule::reimportDesktopVaultSchedule(database, tenant, project, competingIncoming, competingCounter,
             competingFile, QUuid::createUuid(), schedule::VaultSourceFormat::Mspdi, {}, {}, {}, counter, error));
    QVERIFY2(error.contains(QStringLiteral("Schedule changed since it was opened")), qPrintable(error));

    schedule::Project committed;
    QVERIFY2(schedule::loadDesktopVaultSchedule(database, tenant, project, committed, counter, error), qPrintable(error));
    QCOMPARE(counter, qint64(1));
    QCOMPARE(committed.title, incoming.title);
    schedule::VaultSourceMatch competingMatch = schedule::VaultSourceMatch::Matched;
    QUuid competingProject;
    QVERIFY2(schedule::lookupDesktopVaultSource(database, tenant, competingFile, {}, {}, competingMatch,
                                                competingProject, error), qPrintable(error));
    QCOMPARE(competingMatch, schedule::VaultSourceMatch::Unrecognized);
    QVERIFY(competingProject.isNull());
    const QString accessReport = QStringLiteral(
        "SELECT login_name, schedule_visible, can_save, can_reimport FROM "
        "vault.manager_check_desktop_project_access(%1,'%2')").arg(scope, id(actor));
    QVERIFY(execute(accessReport));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), actorRole);
    QVERIFY(sql.value(1).toBool() && sql.value(2).toBool() && sql.value(3).toBool());
    QVERIFY(!sql.next());

    QVERIFY(execute(QStringLiteral("RESET SESSION AUTHORIZATION")));
    QVERIFY(execute(QStringLiteral("SET ROLE %1").arg(ownerRole)));
    QVERIFY(execute(QStringLiteral("SELECT vault.revoke_desktop_schedule_updates('%1')").arg(actorRole)));
    QVERIFY(execute(QStringLiteral(
        "SELECT action, operator_login::text = session_user::text, operator_role::text = current_user::text "
        "FROM vault_admin.desktop_capability_events WHERE target_login = '%1' AND capability = 'schedule_updates' "
        "ORDER BY id").arg(actorRole)));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("enabled"));
    QVERIFY(sql.value(1).toBool() && sql.value(2).toBool());
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("revoked"));
    QVERIFY(sql.value(1).toBool() && sql.value(2).toBool());
    QVERIFY(!sql.next());
    QVERIFY(execute(QStringLiteral("RESET ROLE")));
    QVERIFY(execute(QStringLiteral("SET SESSION AUTHORIZATION %1").arg(actorRole)));
    QVERIFY(execute(accessReport));
    QVERIFY(sql.next());
    QVERIFY(sql.value(1).toBool() && !sql.value(2).toBool() && !sql.value(3).toBool());
    QVERIFY(!sql.next());
    QVERIFY2(schedule::loadDesktopVaultSchedule(database, tenant, project, committed, counter, error), qPrintable(error));
    QCOMPARE(counter, qint64(1));
    QVERIFY(!schedule::saveDesktopVaultSchedule(database, tenant, project, committed, counter,
             {}, {}, {}, deniedCounter, error));
    QVERIFY2(error.contains(QStringLiteral("permission"), Qt::CaseInsensitive), qPrintable(error));
    QVERIFY(!schedule::reimportDesktopVaultSchedule(database, tenant, project, incoming, 1,
             revisedFile, QUuid::createUuid(), schedule::VaultSourceFormat::Mspdi, {}, {}, {}, deniedCounter, error));
    QVERIFY2(error.contains(QStringLiteral("permission"), Qt::CaseInsensitive), qPrintable(error));
    QVERIFY(execute(QStringLiteral("RESET SESSION AUTHORIZATION")));
    QVERIFY(execute(QStringLiteral("SET ROLE %1").arg(ownerRole)));
    QVERIFY(execute(QStringLiteral("SELECT vault.enable_desktop_capability('%1','schedule_updates')").arg(actorRole)));
    QVERIFY(execute(QStringLiteral("RESET ROLE")));
    QVERIFY(execute(QStringLiteral("SET SESSION AUTHORIZATION %1").arg(actorRole)));

    for (const QByteArray &file : {initialFile, revisedFile}) {
        schedule::VaultSourceMatch match = schedule::VaultSourceMatch::Unrecognized;
        QUuid recognizedProject;
        QVERIFY2(schedule::lookupDesktopVaultSource(database, tenant, file, {}, {}, match,
                                                    recognizedProject, error), qPrintable(error));
        QCOMPARE(match, schedule::VaultSourceMatch::Matched);
        QCOMPARE(recognizedProject, project);
    }

    QVERIFY(!schedule::importInitialVaultScheduleAsManager(database, tenant, QUuid::createUuid(), revisedFile,
             schedule::VaultSourceFormat::Mspdi, {}, {}, error));

    QVERIFY(execute(QStringLiteral(
        "SELECT record_id FROM vault.desktop_read_schedule(%1) "
        "WHERE record_kind = 'assignment' AND source_uid = 30").arg(scope)));
    QVERIFY(sql.next());
    QVERIFY(sql.value(0).toUuid() != oldAssignment);

    QVERIFY(execute(QStringLiteral(
        "SELECT id, task_id, assignment_id, work_millis FROM vault.desktop_list_time(%1)").arg(scope)));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toUuid(), entry);
    QCOMPARE(sql.value(1).toUuid(), oldTask);
    QCOMPARE(sql.value(2).toUuid(), oldAssignment);
    QCOMPARE(sql.value(3).toLongLong(), qint64(3600000));
    QVERIFY(!sql.next());
    QVERIFY(execute(QStringLiteral("RESET SESSION AUTHORIZATION")));
    QVERIFY(execute(QStringLiteral("SELECT count(*) FROM vault.projects WHERE organization_id = '%1'").arg(id(tenant))));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toInt(), 1);
    QVERIFY(execute(QStringLiteral("SET SESSION AUTHORIZATION %1").arg(actorRole)));
    verifyMppVaultRoundTrip(database, tenant);
    QVERIFY(execute(QStringLiteral("RESET SESSION AUTHORIZATION")));

    QVERIFY(execute(QStringLiteral(
        "SELECT id FROM vault.projects WHERE organization_id = '%1' AND id <> '%2'").arg(id(tenant), id(project))));
    QVERIFY(sql.next());
    const QUuid otherProject = sql.value(0).toUuid();
    QVERIFY(!otherProject.isNull());
    QVERIFY(execute(QStringLiteral(
        "INSERT INTO vault.project_import_sources (organization_id, project_id, source_namespace, source_key) "
        "VALUES ('%1','%2','test','existing-source')").arg(id(tenant), id(otherProject))));
    QVERIFY(execute(QStringLiteral("SET SESSION AUTHORIZATION %1").arg(actorRole)));
    const QString sourceNamespace = QStringLiteral("test");
    const QString sourceKey = QStringLiteral("existing-source");
    schedule::VaultSourceMatch conflictMatch = schedule::VaultSourceMatch::Unrecognized;
    QUuid conflictingProject;
    QVERIFY2(schedule::lookupDesktopVaultSource(database, tenant, initialFile, sourceNamespace, sourceKey,
                                                conflictMatch, conflictingProject, error), qPrintable(error));
    QCOMPARE(conflictMatch, schedule::VaultSourceMatch::Ambiguous);
    QVERIFY(conflictingProject.isNull());
    schedule::Project otherLive, rejectedIncoming;
    qint64 otherCounter = -1;
    QVERIFY(!schedule::previewDesktopVaultReimport(database, tenant, otherProject, initialFile,
             schedule::VaultSourceFormat::Mspdi, otherLive, rejectedIncoming, otherCounter, error));
    QVERIFY2(error.contains(QStringLiteral("belongs to a different project")), qPrintable(error));

    // The database must also preserve the destination if a caller skips preview.
    QVERIFY2(schedule::loadDesktopVaultSchedule(database, tenant, otherProject, otherLive, otherCounter, error),
             qPrintable(error));
    QVERIFY(!schedule::reimportDesktopVaultSchedule(database, tenant, otherProject, incoming, otherCounter,
             revisedFile, QUuid::createUuid(), schedule::VaultSourceFormat::Mspdi, {}, {}, {}, counter, error));
    QVERIFY2(error.contains(QStringLiteral("Source already registered to another project")), qPrintable(error));
    schedule::Project otherAfter;
    QVERIFY2(schedule::loadDesktopVaultSchedule(database, tenant, otherProject, otherAfter, counter, error),
             qPrintable(error));
    QCOMPARE(counter, otherCounter);
    QVERIFY(otherAfter == otherLive);
    QVERIFY(execute(QStringLiteral("RESET SESSION AUTHORIZATION")));

    // Give the test login manager access through a group alone.
    const QString actorId = id(actor);
    const QString tenantId = id(tenant);
    const QString team = id(QUuid::createUuid());
    const QString recipient = id(QUuid::createUuid());
    QVERIFY(execute(QStringLiteral(
        "UPDATE vault.organization_members SET role = 'member' WHERE organization_id = '%1' AND user_id = '%2'; "
        "DELETE FROM vault.project_members WHERE organization_id = '%1' AND user_id = '%2'; "
        "INSERT INTO vault.groups VALUES ('%1','%3','Managers'); "
        "INSERT INTO vault.group_members VALUES ('%1','%3','%2'); "
        "INSERT INTO vault.users (id, issuer, subject, display_name, home_organization_id) "
        "VALUES ('%4','test','recipient','Recipient','%1'); "
        "INSERT INTO vault.organization_members VALUES ('%1','%4','member',true); "
        "GRANT EXECUTE ON FUNCTION vault.manager_change_project_share(uuid,uuid,text,uuid,text,boolean) TO %5")
        .arg(tenantId, actorId, team, recipient, actorRole)));
    QVERIFY(execute(QStringLiteral("INSERT INTO vault.project_group_shares VALUES (%1,'%2','manager')")
                    .arg(scope, team)));
    QVERIFY(execute(QStringLiteral("SET SESSION AUTHORIZATION %1").arg(actorRole)));
    const QString share = QStringLiteral("SELECT vault.manager_change_project_share(%1,'user','%2','viewer',%3)");
    QVERIFY(execute(share.arg(scope, recipient, QStringLiteral("true"))));
    QVERIFY(sql.next() && sql.value(0).toBool());
    const QString grants = QStringLiteral(
        "SELECT principal_kind, principal_name, project_role FROM vault.manager_list_project_grants(%1) "
        "ORDER BY principal_kind, principal_name").arg(scope);
    QVERIFY(execute(grants));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("group"));
    QCOMPARE(sql.value(1).toString(), QStringLiteral("Managers"));
    QCOMPARE(sql.value(2).toString(), QStringLiteral("manager"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("user"));
    QCOMPARE(sql.value(1).toString(), QStringLiteral("Recipient"));
    QCOMPARE(sql.value(2).toString(), QStringLiteral("viewer"));
    QVERIFY(!sql.next());
    QVERIFY(execute(share.arg(scope, recipient, QStringLiteral("false"))));
    QVERIFY(sql.next() && sql.value(0).toBool());

    QVERIFY(execute(QStringLiteral("RESET SESSION AUTHORIZATION")));
    QVERIFY(execute(QStringLiteral(
        "DELETE FROM vault.group_members WHERE organization_id = '%1' AND group_id = '%2' AND user_id = '%3'")
        .arg(tenantId, team, actorId)));
    QVERIFY(execute(QStringLiteral("SET SESSION AUTHORIZATION %1").arg(actorRole)));
    QVERIFY(!sql.exec(share.arg(scope, recipient, QStringLiteral("true"))));
    QCOMPARE(sql.lastError().nativeErrorCode(), QStringLiteral("42501"));
    QVERIFY(!sql.exec(grants));
    QCOMPARE(sql.lastError().nativeErrorCode(), QStringLiteral("42501"));
    QVERIFY(execute(QStringLiteral("RESET SESSION AUTHORIZATION")));
    QVERIFY(execute(QStringLiteral("INSERT INTO vault.project_members VALUES (%1,'%2','viewer')").arg(scope, actorId)));
    QVERIFY(execute(QStringLiteral("GRANT EXECUTE ON FUNCTION vault.desktop_list_projects(uuid) TO %1").arg(actorRole)));
    QVERIFY(execute(QStringLiteral("SET SESSION AUTHORIZATION %1").arg(actorRole)));
    conflictMatch = schedule::VaultSourceMatch::Unrecognized;
    QVERIFY2(schedule::lookupDesktopVaultSource(database, tenant, initialFile, sourceNamespace, sourceKey,
                                                conflictMatch, conflictingProject, error), qPrintable(error));
    QCOMPARE(conflictMatch, schedule::VaultSourceMatch::Restricted);
    QVERIFY(conflictingProject.isNull());

    QFile restrictedSource(QStringLiteral(SCHEDULEIO_FIXTURE_DIR "/Timeline Test.mpp"));
    QVERIFY(restrictedSource.open(QIODevice::ReadOnly));
    const QByteArray restrictedBytes = restrictedSource.readAll();
    QVERIFY2(schedule::lookupDesktopVaultSource(database, tenant, restrictedBytes, {}, {},
                                                conflictMatch, conflictingProject, error), qPrintable(error));
    QCOMPARE(conflictMatch, schedule::VaultSourceMatch::Restricted);
    QVERIFY(conflictingProject.isNull());
    QVERIFY(!schedule::previewDesktopVaultReimport(database, tenant, project, restrictedBytes,
             schedule::VaultSourceFormat::Mpp, otherLive, rejectedIncoming, otherCounter, error));
    QVERIFY2(error.contains(QStringLiteral("belongs to a restricted project")), qPrintable(error));
    QVERIFY(execute(QStringLiteral(
        "SELECT task_name, starts_at FROM vault.desktop_list_schedule_tasks(%1) WHERE source_uid = 10").arg(scope)));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("Renamed task"));
    QCOMPARE(sql.value(1).toDateTime().toUTC(), incoming.tasks.front().start.toUTC());
    QVERIFY(!sql.next());
    QList<schedule::VaultSharedTask> sharedTasks;
    QVERIFY2(schedule::listDesktopVaultTasks(database, tenant, project, sharedTasks, error), qPrintable(error));
    QCOMPARE(sharedTasks.size(), 2);
    QVERIFY(sharedTasks.front().assignments.isEmpty());
    QVERIFY(sharedTasks.back().assignments.isEmpty());
    QVERIFY(sharedTasks.front().predecessors.isEmpty());
    QCOMPARE(sharedTasks.back().predecessors.size(), 1);
    const auto &predecessor = sharedTasks.back().predecessors.front();
    QCOMPARE(predecessor.sourceUid, 10);
    QCOMPARE(predecessor.linkType, int(schedule::Relation::FinishToStart));
    QCOMPARE(predecessor.lagMillis, qint64(3600000));
    QVERIFY(execute(QStringLiteral("SELECT count(*) FROM vault.desktop_read_schedule(%1)").arg(scope)));
    QVERIFY(sql.next() && sql.value(0).toInt() == 0);
    QVERIFY(execute(QStringLiteral("RESET SESSION AUTHORIZATION")));

    QVERIFY(execute(QStringLiteral(
        "UPDATE vault.project_members SET role = 'contributor' WHERE (organization_id, project_id) = (%1) "
        "AND user_id = '%2'; "
        "INSERT INTO vault.resource_user_mappings (organization_id, project_id, id, resource_id, user_id) "
        "SELECT organization_id, project_id, '%3', id, '%2' FROM vault.resources "
        "WHERE (organization_id, project_id) = (%1) AND source_uid = 20 AND NOT retired")
        .arg(scope, actorId, id(QUuid::createUuid()))));
    QVERIFY(execute(QStringLiteral("SET SESSION AUTHORIZATION %1").arg(actorRole)));
    const QString visibleTasks = QStringLiteral(
        "SELECT source_uid FROM vault.desktop_list_schedule_tasks(%1) ORDER BY source_uid").arg(scope);
    QVERIFY(execute(visibleTasks));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toInt(), 11);
    QVERIFY(!sql.next());
    QVERIFY2(schedule::listDesktopVaultTasks(database, tenant, project, sharedTasks, error), qPrintable(error));
    QCOMPARE(sharedTasks.size(), 1);
    QVERIFY(sharedTasks.front().predecessors.isEmpty());
    QCOMPARE(sharedTasks.front().assignments.size(), 1);
    const auto &visibleAssignment = sharedTasks.front().assignments.front();
    QCOMPARE(visibleAssignment.resourceName, QStringLiteral("Planner"));
    QVERIFY(visibleAssignment.workMillis.has_value());
    QCOMPARE(*visibleAssignment.workMillis, qint64(7200000));

    QVERIFY(execute(QStringLiteral("RESET SESSION AUTHORIZATION")));
    QVERIFY(execute(QStringLiteral(
        "UPDATE vault.resource_user_mappings SET effective_until = clock_timestamp() "
        "WHERE (organization_id, project_id) = (%1) AND user_id = '%2' AND effective_until IS NULL; "
        "INSERT INTO vault.resource_user_mappings (organization_id, project_id, id, resource_id, user_id) "
        "SELECT organization_id, project_id, '%3', id, '%4' FROM vault.resources "
        "WHERE (organization_id, project_id) = (%1) AND source_uid = 20 AND NOT retired")
        .arg(scope, actorId, id(QUuid::createUuid()), recipient)));
    QVERIFY(execute(QStringLiteral("SET SESSION AUTHORIZATION %1").arg(actorRole)));
    QVERIFY(execute(visibleTasks));
    QVERIFY(!sql.next());
    QVERIFY(execute(QStringLiteral("SELECT id FROM vault.desktop_list_time(%1)").arg(scope)));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toUuid(), entry);
    QVERIFY(!sql.next());

    const QString actions = QStringLiteral("SELECT can_edit, can_review, submitter_name, task_name "
                                           "FROM vault.desktop_list_time_actions(%1) "
                                           "WHERE id = '%2'").arg(scope, id(entry));
    QVERIFY(execute(actions));
    QVERIFY(sql.next() && sql.value(0).toBool() && !sql.value(1).toBool());
    const QString transition = QStringLiteral("SELECT vault.desktop_update_time(%1, '%2', '%3', '%4', "
                                              "NULL, NULL, NULL, NULL)");
    QVERIFY(execute(transition.arg(scope, id(entry), QStringLiteral("draft"), QStringLiteral("submitted"))));
    QVERIFY(sql.next() && sql.value(0).toBool());
    QVERIFY(execute(actions));
    QVERIFY(sql.next() && !sql.value(0).toBool() && !sql.value(1).toBool());
    QVERIFY(execute(transition.arg(scope, id(entry), QStringLiteral("submitted"), QStringLiteral("approved"))));
    QVERIFY(sql.next() && !sql.value(0).toBool());

    QVERIFY(execute(QStringLiteral("RESET SESSION AUTHORIZATION")));
    QVERIFY(execute(QStringLiteral("INSERT INTO vault.project_members VALUES (%1,'%2','time_approver'); "
                                    "UPDATE vault.manager_principals SET user_id = '%2' WHERE database_role_name = '%3'")
                    .arg(scope, recipient, actorRole)));
    QVERIFY(execute(QStringLiteral("SET SESSION AUTHORIZATION %1").arg(actorRole)));
    QVERIFY(execute(actions));
    QVERIFY(sql.next() && !sql.value(0).toBool() && sql.value(1).toBool());
    QCOMPARE(sql.value(2).toString(), QStringLiteral("Test operator"));
    QCOMPARE(sql.value(3).toString(), QStringLiteral("First task"));
    QVERIFY(execute(transition.arg(scope, id(entry), QStringLiteral("submitted"), QStringLiteral("approved"))));
    QVERIFY(sql.next() && sql.value(0).toBool());
    QVERIFY(execute(QStringLiteral("RESET SESSION AUTHORIZATION")));

    const QString otherTenant = id(QUuid::createUuid());
    const QString otherScope = QStringLiteral("'%1','%2'").arg(otherTenant, id(project));
    QVERIFY(execute(QStringLiteral(
        "INSERT INTO vault.organizations (id, name) VALUES ('%1','Other tenant'); "
        "INSERT INTO vault.projects (organization_id, id, title) VALUES (%2,'Same project UUID')")
        .arg(otherTenant, otherScope)));
    QVERIFY(!sql.exec(QStringLiteral(
        "INSERT INTO vault.assignments (organization_id, project_id, id, task_id, retired) "
        "VALUES (%1,'%2','%3',true)").arg(otherScope, id(QUuid::createUuid()), id(oldTask))));
    QCOMPARE(sql.lastError().nativeErrorCode(), QStringLiteral("23503"));

    QVERIFY(execute(QStringLiteral("SET SESSION AUTHORIZATION %1").arg(actorRole)));
    QVERIFY(execute(QStringLiteral(
        "SELECT set_config('app.current_organization_id', '%1', false), "
        "set_config('app.current_user_id', '%2', false)").arg(otherTenant, actorId)));
    QVERIFY(execute(QStringLiteral("SELECT * FROM vault.desktop_list_projects('%1')").arg(otherTenant)));
    QVERIFY(!sql.next());
    QVERIFY(execute(QStringLiteral("SELECT * FROM vault.desktop_list_time_actions(%1)").arg(otherScope)));
    QVERIFY(!sql.next());
    QVERIFY(execute(QStringLiteral("SELECT status FROM vault.desktop_list_time_actions(%1)").arg(scope)));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("approved"));
    QVERIFY(!sql.next());
    QVERIFY(execute(QStringLiteral("RESET SESSION AUTHORIZATION")));
}

QTEST_GUILESS_MAIN(VaultReimportTest)
#include "tst_vault_reimport.moc"
