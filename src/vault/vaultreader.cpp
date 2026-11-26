// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#include "vaultreader.h"
#include "mppio.h"
#include "xmlio.h"
#include "projectmetadata.h"
#include "importmatch.h"
#include "schedulecodec.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <utility>

namespace schedule {
namespace {

bool query(QSqlQuery &statement, const QString &sql, const QVariantList &parameters,
           QString &error)
{
    statement.setForwardOnly(true);
    if (!statement.prepare(sql)) {
        error = statement.lastError().text();
        return false;
    }
    for (const QVariant &parameter : parameters)
        statement.addBindValue(parameter);
    if (!statement.exec()) {
        error = statement.lastError().text();
        return false;
    }
    return true;
}

template<class Entity>
bool loadRows(const QSqlDatabase &database, const QString &sql, const QVariantList &scope,
              bool (*decode)(const QByteArray &, Entity &), QList<Entity> &rows, QString &error)
{
    QSqlQuery statement(database);
    if (!query(statement, sql, scope, error))
        return false;

    while (statement.next()) {
        Entity entity;
        if (!decode(statement.value(0).toByteArray(), entity)) {
            error = QStringLiteral("Invalid Vault schedule row");
            return false;
        }
        rows.append(std::move(entity));
    }
    if (statement.lastError().isValid()) {
        error = statement.lastError().text();
        return false;
    }
    return true;
}

bool readProject(const QSqlDatabase &database, const QVariantList &scope,
                 Project &project, qint64 &counter, QString &error)
{
    QSqlQuery statement(database);
    if (!query(statement, QStringLiteral(
            "SELECT p.edit_counter, d.format_revision, d.project_metadata, d.project_collections "
            "FROM vault.projects p JOIN vault.schedule_data d "
            "ON (d.organization_id, d.project_id) = (p.organization_id, p.id) "
            "WHERE p.organization_id = CAST(? AS uuid) AND p.id = CAST(? AS uuid)"),
            scope, error))
        return false;

    if (!statement.next()) {
        error = statement.lastError().isValid() ? statement.lastError().text()
                                                : QStringLiteral("Vault schedule unavailable");
        return false;
    }
    counter = statement.value(0).toLongLong();
    if (statement.value(1).toInt() != 1
        || !decodeVaultProjectMetadata(statement.value(2).toByteArray(), project)
        || !decodeVaultProjectCollections(statement.value(3).toByteArray(), project)) {
        error = QStringLiteral("Unsupported or invalid Vault schedule data");
        return false;
    }
    return true;
}

bool setContext(const QSqlDatabase &database, const QVariant &tenant,
                const QUuid &actorId, QString &error)
{
    QSqlQuery statement(database);
    return query(statement, QStringLiteral(
        "SELECT set_config('app.current_organization_id', ?, true), "
        "set_config('app.current_user_id', ?, true)"),
        {tenant, actorId.toString(QUuid::WithoutBraces)}, error);
}

QString sourceDigest(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool parseSource(const QByteArray &sourceBytes, VaultSourceFormat format,
                 Project &incoming, QString &error)
{
    if (format == VaultSourceFormat::Mpp) {
        MppIO reader;
        if (!reader.openFromData(sourceBytes)) {
            error = reader.errorString();
            return false;
        }
        incoming = reader.project();
    } else {
        XmlIO reader;
        if (!reader.openFromData(sourceBytes)) {
            error = reader.errorString();
            return false;
        }
        incoming = reader.project();
    }

    return true;
}

using IdIndex = QHash<int, QString>;

bool loadIds(const QSqlDatabase &database, const QString &sql,
             const QVariantList &scope, IdIndex &ids, QString &error)
{
    QSqlQuery statement(database);
    if (!query(statement, sql, scope, error))
        return false;
    while (statement.next())
        ids.insert(statement.value(0).toInt(), statement.value(1).toString());

    if (statement.lastError().isValid()) {
        error = statement.lastError().text();
        return false;
    }
    return true;
}

QString resolveId(int uid, const QSet<int> &matches, const IdIndex &live)
{
    if (matches.contains(uid))
        return live.value(uid);
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonValue nullableId(const QString &id)
{
    return id.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(id);
}

template<class Entity>
QJsonObject row(const Entity &entity, const QString &id, int position, const QByteArray &data)
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("uid"), entity.uniqueId < 0 ? QJsonValue(QJsonValue::Null)
                                                       : QJsonValue(entity.uniqueId)},
            {QStringLiteral("position"), position},
            {QStringLiteral("model_data"), QString::fromLatin1(data.toBase64())},
            {QStringLiteral("properties"), QJsonObject{}}};
}

QString json(const QJsonArray &array)
{
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

bool appendPayload(QVariantList &parameters, const Project &incoming,
                   const VaultImportMatches &matches, const IdIndex &liveTasks,
                   const IdIndex &liveResources, const IdIndex &liveAssignments, QString &error)
{
    IdIndex taskIds, resourceIds;
    QList<QPair<int, QString>> outline;
    QJsonArray tasks, resources, assignments;
    QHash<int, QJsonArray> predecessors;
    for (const Relation &relation : incoming.relations) {
        predecessors[relation.successorTaskUid].append(QJsonObject{
            {QStringLiteral("predecessor_uid"), relation.predecessorTaskUid},
            {QStringLiteral("link_type"), relation.type},
            {QStringLiteral("lag_millis"), QString::number(relation.lagMillis)}});
    }
    for (const Task &task : incoming.tasks) {
        const QString id = resolveId(task.uniqueId, matches.tasks, liveTasks);
        while (!outline.isEmpty() && outline.constLast().first >= task.outlineLevel)
            outline.removeLast();
        QJsonObject values = row(task, id, tasks.size(), encodeVaultTask(task));
        values.insert(QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("name"), task.name},
            {QStringLiteral("start"), task.start.toUTC().toString(Qt::ISODateWithMs)},
            {QStringLiteral("finish"), task.finish.toUTC().toString(Qt::ISODateWithMs)},
            {QStringLiteral("percent_complete"), task.percentComplete},
            {QStringLiteral("milestone"), task.milestone},
            {QStringLiteral("summary"), task.summary},
            {QStringLiteral("predecessors"), predecessors.value(task.uniqueId)}});
        values.insert(QStringLiteral("parent_id"), nullableId(
            outline.isEmpty() ? QString() : outline.constLast().second));
        tasks.append(values);
        outline.append({task.outlineLevel, id});
        if (task.uniqueId >= 0)
            taskIds.insert(task.uniqueId, id);
    }
    for (const Resource &resource : incoming.resources) {
        const QString id = resolveId(resource.uniqueId, matches.resources, liveResources);
        QJsonObject values = row(resource, id, resources.size(), encodeVaultResource(resource));
        const QString kind = resource.type == Resource::Type::Work
            ? (isVaultMappingCandidate(resource) ? QStringLiteral("work")
                : resource.budget ? QStringLiteral("budget") : QStringLiteral("placeholder"))
            : resource.type == Resource::Type::Material ? QStringLiteral("material") : QStringLiteral("cost");
        values.insert(QStringLiteral("properties"),
                      QJsonObject{{QStringLiteral("kind"), kind}, {QStringLiteral("name"), resource.name}});
        resources.append(values);
        if (resource.uniqueId >= 0)
            resourceIds.insert(resource.uniqueId, id);
    }
    for (const Assignment &assignment : incoming.assignments) {
        const QString id = resolveId(assignment.uniqueId, matches.assignments, liveAssignments);
        const QString taskId = taskIds.value(assignment.taskUniqueId);
        const QString resourceId = resourceIds.value(assignment.resourceUniqueId);
        if ((assignment.taskUniqueId >= 0 && taskId.isEmpty())
            || (assignment.resourceUniqueId >= 0 && resourceId.isEmpty())) {
            error = QStringLiteral("Vault assignment references a missing task or resource");
            return false;
        }
        QJsonObject values = row(assignment, id, assignments.size(), encodeVaultAssignment(assignment));
        values.insert(QStringLiteral("task_id"), nullableId(taskId));
        values.insert(QStringLiteral("resource_id"), nullableId(resourceId));
        if (!assignment.budget) {
            values.insert(QStringLiteral("properties"), QJsonObject{
                {QStringLiteral("work_millis"), QString::number(assignment.workMillis)},
                {QStringLiteral("actual_work_millis"), QString::number(assignment.actualWorkMillis)},
                {QStringLiteral("remaining_work_millis"), QString::number(assignment.remainingWorkMillis)}});
        }
        assignments.append(values);
    }

    parameters << encodeVaultProjectMetadata(incoming) << encodeVaultProjectCollections(incoming)
               << json(tasks) << json(resources) << json(assignments);
    return true;
}

bool executeSave(const QSqlDatabase &database, const QString &operation,
                 const QVariantList &parameters, qint64 &counter, QString &error,
                 bool bindReceipt = false)
{
    QSqlQuery statement(database);
    if (!query(statement, QStringLiteral(
        "SELECT %1(CAST(? AS uuid), CAST(? AS uuid), CAST(? AS bigint), "
        "CAST(? AS text), CAST(? AS bytea), CAST(? AS bytea), CAST(? AS jsonb), "
        "CAST(? AS jsonb), CAST(? AS jsonb)%2)").arg(operation,
            bindReceipt ? QStringLiteral(", CAST(? AS text), CAST(? AS uuid), CAST(? AS text)") : QString()), parameters, error))
        return false;

    if (!statement.next() || statement.value(0).isNull()) {
        error = statement.lastError().isValid() ? statement.lastError().text()
            : QStringLiteral("Schedule changed or is unavailable for editing");
        return false;
    }
    counter = statement.value(0).toLongLong();
    return true;
}

bool writeRows(const QSqlDatabase &database, QVariantList scope, qint64 expected,
               const Project &incoming, const VaultImportMatches &matches,
               qint64 &counter, QString &error)
{
    {
        QSqlQuery current(database);
        if (!query(current, QStringLiteral(
            "SELECT edit_counter FROM vault.projects WHERE organization_id = CAST(? AS uuid) "
            "AND id = CAST(? AS uuid)"), scope, error))
            return false;
        if (!current.next() || current.value(0).toLongLong() != expected) {
            error = current.lastError().isValid() ? current.lastError().text()
                : QStringLiteral("Schedule changed or is unavailable for editing");
            return false;
        }
    }

    IdIndex liveTasks, liveResources, liveAssignments;
    if (!loadIds(database, QStringLiteral(
            "SELECT source_uid, id::text FROM vault.tasks WHERE organization_id = CAST(? AS uuid) "
            "AND project_id = CAST(? AS uuid) AND NOT retired AND source_uid IS NOT NULL"),
            scope, liveTasks, error)
        || !loadIds(database, QStringLiteral(
            "SELECT source_uid, id::text FROM vault.resources WHERE organization_id = CAST(? AS uuid) "
            "AND project_id = CAST(? AS uuid) AND NOT retired AND source_uid IS NOT NULL"),
            scope, liveResources, error)
        || !loadIds(database, QStringLiteral(
            "SELECT source_uid, id::text FROM vault.assignments WHERE organization_id = CAST(? AS uuid) "
            "AND project_id = CAST(? AS uuid) AND NOT retired AND source_uid IS NOT NULL"),
            scope, liveAssignments, error))
        return false;

    scope << expected << (incoming.title.isNull() ? QStringLiteral("") : incoming.title);
    if (!appendPayload(scope, incoming, matches, liveTasks, liveResources, liveAssignments, error))
        return false;

    return executeSave(database, QStringLiteral("vault.save_schedule"), scope, counter, error);
}

struct DesktopSnapshot
{
    Project project;
    qint64 counter = 0;
    IdIndex tasks;
    IdIndex resources;
    IdIndex assignments;
};

template<class Entity>
bool decodeDesktopRow(const QSqlQuery &row, QList<Entity> &entities, IdIndex &ids,
                      bool (*decode)(const QByteArray &, Entity &))
{
    Entity entity;
    if (!decode(row.value(6).toByteArray(), entity))
        return false;
    if (!row.value(2).isNull())
        ids.insert(row.value(2).toInt(), row.value(3).toUuid().toString(QUuid::WithoutBraces));
    entities.append(std::move(entity));
    return true;
}

bool readDesktopSnapshot(const QSqlDatabase &database, const QVariantList &scope,
                         DesktopSnapshot &snapshot, QString &error)
{
    QSqlQuery statement(database);
    if (!query(statement, QStringLiteral(
            "SELECT record_kind, row_position, source_uid, record_id, edit_counter, "
            "format_revision, model_data FROM vault.desktop_read_schedule(CAST(? AS uuid), "
            "CAST(? AS uuid)) ORDER BY record_kind, row_position, record_id"), scope, error))
        return false;

    bool metadata = false, collections = false;
    while (statement.next()) {
        const QString kind = statement.value(0).toString();
        bool valid = false;
        if (kind == QStringLiteral("metadata")) {
            metadata = !metadata && statement.value(5).toInt() == 1
                && decodeVaultProjectMetadata(statement.value(6).toByteArray(), snapshot.project);
            snapshot.counter = statement.value(4).toLongLong();
            valid = metadata;
        } else if (kind == QStringLiteral("collections")) {
            collections = !collections
                && decodeVaultProjectCollections(statement.value(6).toByteArray(), snapshot.project);
            valid = collections;
        } else if (kind == QStringLiteral("task")) {
            valid = decodeDesktopRow(statement, snapshot.project.tasks, snapshot.tasks, decodeVaultTask);
        } else if (kind == QStringLiteral("resource")) {
            valid = decodeDesktopRow(statement, snapshot.project.resources, snapshot.resources, decodeVaultResource);
        } else if (kind == QStringLiteral("assignment")) {
            valid = decodeDesktopRow(statement, snapshot.project.assignments, snapshot.assignments, decodeVaultAssignment);
        }
        if (!valid) {
            error = QStringLiteral("Unsupported or invalid Vault schedule data");
            return false;
        }
    }
    if (statement.lastError().isValid()) {
        error = statement.lastError().text();
        return false;
    }
    if (!metadata || !collections) {
        error = QStringLiteral("Vault schedule unavailable");
        return false;
    }
    return true;
}

bool desktopScope(const QSqlDatabase &database, const QUuid &tenant, const QUuid &project,
                  QVariantList &scope, QString &error)
{
    error.clear();
    if (!database.isOpen() || database.driverName() != QStringLiteral("QPSQL")
        || tenant.isNull() || project.isNull()) {
        error = QStringLiteral("An open PostgreSQL connection and Vault identities are required");
        return false;
    }
    scope = {tenant.toString(QUuid::WithoutBraces), project.toString(QUuid::WithoutBraces)};
    return true;
}

} // namespace

bool loadVaultSchedule(const QSqlDatabase &database, const QUuid &organizationId,
                       const QUuid &actorId, const QUuid &projectId, Project &project,
                       qint64 &editCounter, QString &error)
{
    error.clear();
    if (!database.isOpen() || database.driverName() != QStringLiteral("QPSQL")
        || organizationId.isNull() || actorId.isNull() || projectId.isNull()) {
        error = QStringLiteral("An open PostgreSQL connection and Vault identities are required");
        return false;
    }

    QSqlQuery begin(database);
    if (!begin.exec(QStringLiteral("BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY"))) {
        error = begin.lastError().text();
        return false;
    }
    QSqlDatabase transaction = database;
    const QVariantList scope{organizationId.toString(QUuid::WithoutBraces),
                             projectId.toString(QUuid::WithoutBraces)};
    Project loaded;
    qint64 counter = 0;
    if (!setContext(database, scope.at(0), actorId, error) || !readProject(database, scope, loaded, counter, error)
        || !loadRows(database, QStringLiteral(
            "SELECT model_data FROM vault.tasks WHERE organization_id = CAST(? AS uuid) "
            "AND project_id = CAST(? AS uuid) AND NOT retired ORDER BY position, id"),
            scope, decodeVaultTask, loaded.tasks, error)
        || !loadRows(database, QStringLiteral(
            "SELECT model_data FROM vault.resources WHERE organization_id = CAST(? AS uuid) "
            "AND project_id = CAST(? AS uuid) AND NOT retired ORDER BY position, id"),
            scope, decodeVaultResource, loaded.resources, error)
        || !loadRows(database, QStringLiteral(
            "SELECT model_data FROM vault.assignments WHERE organization_id = CAST(? AS uuid) "
            "AND project_id = CAST(? AS uuid) AND NOT retired ORDER BY position, id"),
            scope, decodeVaultAssignment, loaded.assignments, error)) {
        transaction.rollback();
        return false;
    }

    if (!transaction.commit()) {
        error = transaction.lastError().text();
        transaction.rollback();
        return false;
    }

    project = std::move(loaded);
    editCounter = counter;
    return true;
}

bool saveVaultSchedule(const QSqlDatabase &database, const QUuid &organizationId,
                       const QUuid &actorId, const QUuid &projectId, const Project &incoming,
                       qint64 expectedEditCounter, const QSet<int> &reusedTasks,
                       const QSet<int> &reusedResources, const QSet<int> &reusedAssignments,
                       qint64 &newEditCounter, QString &error)
{
    Project live;
    qint64 loadedCounter = 0;
    if (!loadVaultSchedule(database, organizationId, actorId, projectId, live, loadedCounter, error))
        return false;
    if (loadedCounter != expectedEditCounter) {
        error = QStringLiteral("Schedule changed since it was opened");
        return false;
    }

    VaultImportMatches matches;
    if (!matchVaultImport(live, incoming, reusedTasks, reusedResources, reusedAssignments, matches, error))
        return false;

    QSqlDatabase transaction = database;
    QSqlQuery begin(database);
    if (!begin.exec(QStringLiteral("BEGIN ISOLATION LEVEL REPEATABLE READ"))) {
        error = begin.lastError().text();
        return false;
    }
    const QVariantList scope{organizationId.toString(QUuid::WithoutBraces),
                             projectId.toString(QUuid::WithoutBraces)};
    qint64 counter = 0;
    if (!setContext(database, scope.at(0), actorId, error)
        || !writeRows(database, scope, expectedEditCounter, incoming, matches, counter, error)) {
        transaction.rollback();
        return false;
    }
    if (!transaction.commit()) {
        error = transaction.lastError().text();
        transaction.rollback();
        return false;
    }
    newEditCounter = counter;
    return true;
}

static bool prepareInitialSchedule(const QByteArray &source, VaultSourceFormat format, Project &schedule,
                                   VaultImportMatches &matches, QString &error)
{
    return parseSource(source, format, schedule, error)
        && matchVaultImport(Project{}, schedule, {}, {}, {}, matches, error);
}

bool isVaultMappingCandidate(const Resource &resource)
{
    return resource.type == Resource::Type::Work && !resource.budget
        && (resource.uniqueId != 0 || !resource.name.trimmed().isEmpty());
}

bool inspectInitialVaultSchedule(const QByteArray &sourceBytes, VaultSourceFormat format, Project &project, QString &error)
{
    error.clear();
    Project incoming;
    VaultImportMatches matches;
    QVariantList payload;
    if (!prepareInitialSchedule(sourceBytes, format, incoming, matches, error)
        || !appendPayload(payload, incoming, matches, {}, {}, {}, error))
        return false;
    project = std::move(incoming);
    return true;
}

static bool importInitialSchedule(const QSqlDatabase &database, const QUuid &organizationId,
                                const QUuid &actorId, const QUuid &projectId,
                                const QByteArray &sourceBytes, VaultSourceFormat format,
                                const QString &sourceNamespace, const QString &sourceKey,
                                bool managerLogin, QString &error)
{
    error.clear();
    if (!database.isOpen() || database.driverName() != QStringLiteral("QPSQL")
        || organizationId.isNull() || (!managerLogin && actorId.isNull()) || projectId.isNull()) {
        error = QStringLiteral("An open PostgreSQL connection and Vault identities are required");
        return false;
    }

    Project incoming;
    VaultImportMatches matches;
    if (!prepareInitialSchedule(sourceBytes, format, incoming, matches, error))
        return false;

    const QString tenant = organizationId.toString(QUuid::WithoutBraces);
    QVariantList parameters{tenant, projectId.toString(QUuid::WithoutBraces),
        incoming.title.isNull() ? QStringLiteral("") : incoming.title,
        sourceNamespace, sourceKey, sourceDigest(sourceBytes)};
    if (!appendPayload(parameters, incoming, matches, {}, {}, {}, error))
        return false;

    QSqlDatabase transaction = database;
    if (!transaction.transaction()) {
        error = transaction.lastError().text();
        return false;
    }

    bool imported = false;
    if (managerLogin || setContext(database, tenant, actorId, error)) {
        QSqlQuery statement(database);
        const QString operation = managerLogin ? QStringLiteral("vault.manager_import_initial_schedule")
                                               : QStringLiteral("vault.import_initial_schedule");
        imported = query(statement, QStringLiteral(
            "SELECT %1(CAST(? AS uuid), CAST(? AS uuid), CAST(? AS text), CAST(? AS text), "
            "CAST(? AS text), CAST(? AS text), CAST(? AS bytea), CAST(? AS bytea), CAST(? AS jsonb), "
            "CAST(? AS jsonb), CAST(? AS jsonb))").arg(operation), parameters, error);
    }
    if (!imported || !transaction.commit()) {
        if (imported)
            error = transaction.lastError().text();
        transaction.rollback();
        return false;
    }
    return true;
}

bool importInitialVaultSchedule(const QSqlDatabase &database, const QUuid &organizationId,
                                const QUuid &actorId, const QUuid &projectId,
                                const QByteArray &sourceBytes, VaultSourceFormat format,
                                const QString &sourceNamespace, const QString &sourceKey, QString &error)
{
    return importInitialSchedule(database, organizationId, actorId, projectId, sourceBytes,
                                 format, sourceNamespace, sourceKey, false, error);
}

bool importInitialVaultScheduleAsManager(const QSqlDatabase &database, const QUuid &organizationId,
                                         const QUuid &projectId, const QByteArray &sourceBytes,
                                         VaultSourceFormat format, const QString &sourceNamespace,
                                         const QString &sourceKey, QString &error)
{
    return importInitialSchedule(database, organizationId, {}, projectId, sourceBytes,
                                 format, sourceNamespace, sourceKey, true, error);
}

static bool lookupSource(const QSqlDatabase &database, const QUuid &organizationId,
                       const QUuid &actorId, const QByteArray &sourceBytes,
                       const QString &sourceNamespace, const QString &sourceKey,
                       VaultSourceMatch &match, QUuid &projectId, bool desktop, QString &error)
{
    error.clear();
    if (!database.isOpen() || database.driverName() != QStringLiteral("QPSQL")
        || organizationId.isNull() || (!desktop && actorId.isNull())) {
        error = QStringLiteral("An open PostgreSQL connection and Vault identities are required");
        return false;
    }

    QSqlDatabase transaction = database;
    if (!transaction.transaction()) {
        error = transaction.lastError().text();
        return false;
    }

    const QString tenant = organizationId.toString(QUuid::WithoutBraces);
    QString outcome;
    QUuid foundProject;
    if (desktop || setContext(database, tenant, actorId, error)) {
        QSqlQuery statement(database);
        const QString operation = desktop ? QStringLiteral("vault.manager_lookup_import_source")
                                          : QStringLiteral("vault.lookup_import_source");
        if (query(statement, QStringLiteral(
                "SELECT outcome, project_id FROM %1("
                "CAST(? AS uuid), CAST(? AS text), CAST(? AS text), CAST(? AS text))").arg(operation),
                {tenant, sourceDigest(sourceBytes), sourceNamespace, sourceKey}, error)
            && statement.next()) {
            outcome = statement.value(0).toString();
            foundProject = statement.value(1).toUuid();
        } else if (error.isEmpty()) {
            error = statement.lastError().isValid() ? statement.lastError().text()
                : QStringLiteral("Vault source lookup returned no result");
        }
    }

    VaultSourceMatch found;
    if (outcome == QStringLiteral("unrecognized"))
        found = VaultSourceMatch::Unrecognized;
    else if (outcome == QStringLiteral("matched") && !foundProject.isNull())
        found = VaultSourceMatch::Matched;
    else if (outcome == QStringLiteral("restricted"))
        found = VaultSourceMatch::Restricted;
    else if (outcome == QStringLiteral("ambiguous"))
        found = VaultSourceMatch::Ambiguous;
    else {
        if (error.isEmpty())
            error = QStringLiteral("Unsupported Vault source lookup result");
        transaction.rollback();
        return false;
    }

    if (!transaction.commit()) {
        error = transaction.lastError().text();
        transaction.rollback();
        return false;
    }
    match = found;
    projectId = foundProject;
    return true;
}

bool lookupVaultSource(const QSqlDatabase &database, const QUuid &organizationId,
                       const QUuid &actorId, const QByteArray &sourceBytes,
                       const QString &sourceNamespace, const QString &sourceKey,
                       VaultSourceMatch &match, QUuid &projectId, QString &error)
{
    return lookupSource(database, organizationId, actorId, sourceBytes,
                        sourceNamespace, sourceKey, match, projectId, false, error);
}

bool lookupDesktopVaultSource(const QSqlDatabase &database, const QUuid &organizationId,
                              const QByteArray &sourceBytes, const QString &sourceNamespace,
                              const QString &sourceKey, VaultSourceMatch &match, QUuid &projectId,
                              QString &error)
{
    return lookupSource(database, organizationId, {}, sourceBytes, sourceNamespace, sourceKey,
                        match, projectId, true, error);
}

static bool previewReimport(const QSqlDatabase &database, const QUuid &organizationId,
                          const QUuid &actorId, const QUuid &selectedProjectId,
                          const QByteArray &sourceBytes, VaultSourceFormat format,
                          const QString &sourceNamespace, const QString &sourceKey,
                          Project &live, Project &incoming, QUuid &projectId,
                          qint64 &expectedEditCounter, bool desktop, QString &error)
{
    VaultSourceMatch match;
    QUuid found;
    if (!lookupSource(database, organizationId, actorId, sourceBytes,
                           sourceNamespace, sourceKey, match, found, desktop, error))
        return false;

    if (match == VaultSourceMatch::Restricted) {
        error = QStringLiteral("The import source belongs to a restricted project");
        return false;
    }

    if (match == VaultSourceMatch::Ambiguous) {
        error = QStringLiteral("File content and source identifier match different projects");
        return false;
    }

    if (match == VaultSourceMatch::Matched && !selectedProjectId.isNull()
        && found != selectedProjectId) {
        error = QStringLiteral("The import source belongs to a different project");
        return false;
    }

    const QUuid target = match == VaultSourceMatch::Matched ? found : selectedProjectId;
    if (target.isNull()) {
        error = QStringLiteral("Select an existing project for this unrecognized source");
        return false;
    }

    Project loaded, parsed;
    qint64 counter = 0;
    if (!parseSource(sourceBytes, format, parsed, error)
        || !(desktop ? loadDesktopVaultSchedule(database, organizationId, target, loaded, counter, error)
                     : loadVaultSchedule(database, organizationId, actorId, target, loaded, counter, error)))
        return false;

    VaultImportMatches matches;
    if (!matchVaultImport(loaded, parsed, {}, {}, {}, matches, error))
        return false;

    live = std::move(loaded);
    incoming = std::move(parsed);
    projectId = target;
    expectedEditCounter = counter;
    return true;
}

bool previewVaultReimport(const QSqlDatabase &database, const QUuid &organizationId,
                          const QUuid &actorId, const QUuid &selectedProjectId,
                          const QByteArray &sourceBytes, VaultSourceFormat format,
                          const QString &sourceNamespace, const QString &sourceKey,
                          Project &live, Project &incoming, QUuid &projectId,
                          qint64 &expectedEditCounter, QString &error)
{
    return previewReimport(database, organizationId, actorId, selectedProjectId, sourceBytes,
                           format, sourceNamespace, sourceKey, live, incoming, projectId,
                           expectedEditCounter, false, error);
}

bool previewDesktopVaultReimport(const QSqlDatabase &database, const QUuid &organizationId,
                                 const QUuid &selectedProjectId, const QByteArray &sourceBytes,
                                 VaultSourceFormat format, Project &live, Project &incoming,
                                 qint64 &expectedEditCounter, QString &error)
{
    QUuid projectId;
    return previewReimport(database, organizationId, {}, selectedProjectId, sourceBytes, format,
                           {}, {}, live, incoming, projectId, expectedEditCounter, true, error);
}

bool listDesktopVaultTasks(const QSqlDatabase &database, const QUuid &organizationId, const QUuid &projectId,
                           QList<VaultSharedTask> &tasks, QString &error)
{
    QVariantList scope;
    if (!desktopScope(database, organizationId, projectId, scope, error))
        return false;

    QSqlQuery statement(database);
    if (!query(statement, QStringLiteral(
            "SELECT task.task_id, parent_id, task_name, source_uid, starts_at, finishes_at, percent_complete, "
            "link.predecessor_id, link.predecessor_name, link.predecessor_uid, link.link_type, link.lag_millis, "
            "work.assignments "
            "FROM vault.desktop_list_schedule_tasks(?::uuid, ?::uuid) WITH ORDINALITY AS task "
            "LEFT JOIN vault.desktop_list_schedule_dependencies(?::uuid, ?::uuid) AS link "
            "ON link.task_id = task.task_id "
            "LEFT JOIN (SELECT task_id, jsonb_agg(jsonb_build_object('id', assignment_id, "
            "'resource_name', resource_name, 'work_millis', work_millis::text) ORDER BY ordinality)::text AS assignments "
            "FROM vault.desktop_list_schedule_assignments(?::uuid, ?::uuid) WITH ORDINALITY GROUP BY task_id) AS work "
            "ON work.task_id = task.task_id "
            "ORDER BY task.ordinality, link.predecessor_uid, link.predecessor_id, link.link_type, link.lag_millis"),
            scope + scope + scope, error))
        return false;

    QList<VaultSharedTask> rows;
    while (statement.next()) {
        const auto id = statement.value(0).toUuid();
        if (rows.isEmpty() || rows.constLast().id != id) {
            VaultSharedTask task;
            task.id = id;
            task.parentId = statement.value(1).toUuid();
            task.name = statement.value(2).toString();
            if (!statement.isNull(3))
                task.sourceUid = statement.value(3).toInt();
            task.start = statement.value(4).toDateTime();
            task.finish = statement.value(5).toDateTime();
            if (!statement.isNull(6))
                task.percentComplete = statement.value(6).toDouble();
            const auto visibleAssignments = QJsonDocument::fromJson(statement.value(12).toByteArray()).array();
            for (const auto &entry : visibleAssignments) {
                const auto fields = entry.toObject();
                VaultSharedAssignment assignment;
                assignment.id = QUuid(fields.value(QStringLiteral("id")).toString());
                assignment.resourceName = fields.value(QStringLiteral("resource_name")).toString();
                bool hasWork = false;
                const qint64 work = fields.value(QStringLiteral("work_millis")).toString().toLongLong(&hasWork);
                if (hasWork)
                    assignment.workMillis = work;
                task.assignments.append(std::move(assignment));
            }
            rows.append(std::move(task));
        }
        if (!statement.isNull(7)) {
            rows.last().predecessors.append(VaultSharedPredecessor{
                statement.value(7).toUuid(), statement.value(8).toString(), statement.value(9).toInt(),
                statement.value(10).toInt(), statement.value(11).toLongLong()});
        }
    }
    if (statement.lastError().isValid()) {
        error = statement.lastError().text();
        return false;
    }
    tasks = std::move(rows);
    return true;
}

bool loadDesktopVaultSchedule(const QSqlDatabase &database, const QUuid &organizationId,
                              const QUuid &projectId, Project &project, qint64 &editCounter,
                              QString &error)
{
    QVariantList scope;
    DesktopSnapshot snapshot;
    if (!desktopScope(database, organizationId, projectId, scope, error)
        || !readDesktopSnapshot(database, scope, snapshot, error))
        return false;
    project = std::move(snapshot.project);
    editCounter = snapshot.counter;
    return true;
}

static bool saveDesktopSchedule(const QSqlDatabase &database, const QUuid &organizationId,
                              const QUuid &projectId, const Project &incoming, qint64 expectedEditCounter,
                              const QSet<int> &reusedTasks, const QSet<int> &reusedResources,
                              const QSet<int> &reusedAssignments, const QByteArray &importBytes,
                              const QUuid &request, VaultSourceFormat sourceFormat,
                              qint64 &newEditCounter, QString &error)
{
    QVariantList parameters;
    DesktopSnapshot snapshot;
    if (!desktopScope(database, organizationId, projectId, parameters, error))
        return false;
    const bool reimport = !importBytes.isNull();
    const QString digest = reimport ? sourceDigest(importBytes) : QString();
    if (reimport) {
        QSqlQuery receipt(database);
        auto keys = parameters;
        keys << request.toString(QUuid::WithoutBraces) << digest;
        if (!query(receipt, QStringLiteral(
                "SELECT vault.desktop_reimport_receipt(CAST(? AS uuid), CAST(? AS uuid), "
                "CAST(? AS uuid), CAST(? AS text))"), keys, error))
            return false;
        if (receipt.next() && !receipt.value(0).isNull()) {
            newEditCounter = receipt.value(0).toLongLong();
            return true;
        }
        if (receipt.lastError().isValid()) {
            error = receipt.lastError().text();
            return false;
        }
    }
    if (!readDesktopSnapshot(database, parameters, snapshot, error))
        return false;
    if (snapshot.counter != expectedEditCounter) {
        error = QStringLiteral("Schedule changed since it was opened");
        return false;
    }
    VaultImportMatches matches;
    if (!matchVaultImport(snapshot.project, incoming, reusedTasks, reusedResources, reusedAssignments,
                          matches, error))
        return false;

    parameters << expectedEditCounter << (incoming.title.isNull() ? QStringLiteral("") : incoming.title);
    if (!appendPayload(parameters, incoming, matches, snapshot.tasks, snapshot.resources,
                       snapshot.assignments, error))
        return false;
    if (reimport)
        parameters << digest << request.toString(QUuid::WithoutBraces)
                   << (sourceFormat == VaultSourceFormat::Mpp ? QStringLiteral("mpp") : QStringLiteral("mspdi"));
    return executeSave(database, reimport ? QStringLiteral("vault.desktop_commit_reimport")
                                          : QStringLiteral("vault.desktop_save_schedule"),
                       parameters, newEditCounter, error, reimport);
}

bool saveDesktopVaultSchedule(const QSqlDatabase &database, const QUuid &organizationId,
                              const QUuid &projectId, const Project &incoming, qint64 expectedEditCounter,
                              const QSet<int> &reusedTasks, const QSet<int> &reusedResources,
                              const QSet<int> &reusedAssignments, qint64 &newEditCounter, QString &error)
{
    return saveDesktopSchedule(database, organizationId, projectId, incoming, expectedEditCounter,
                               reusedTasks, reusedResources, reusedAssignments, {}, {}, VaultSourceFormat::Mspdi, newEditCounter, error);
}

bool reimportDesktopVaultSchedule(const QSqlDatabase &database, const QUuid &organizationId,
                                  const QUuid &projectId, const Project &incoming, qint64 expectedEditCounter,
                                  const QByteArray &sourceBytes, const QUuid &requestId, VaultSourceFormat sourceFormat,
                                  const QSet<int> &reusedTasks,
                                  const QSet<int> &reusedResources, const QSet<int> &reusedAssignments,
                                  qint64 &newEditCounter, QString &error)
{
    if (sourceBytes.isEmpty() || requestId.isNull()) {
        error = QStringLiteral("Repeat import requires source bytes and a request ID");
        return false;
    }
    return saveDesktopSchedule(database, organizationId, projectId, incoming, expectedEditCounter,
                               reusedTasks, reusedResources, reusedAssignments, sourceBytes,
                               requestId, sourceFormat, newEditCounter, error);
}

} // namespace schedule
