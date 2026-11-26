// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#include "importmatch.h"
#include "model/project.h"

#include <QHash>

namespace schedule {
namespace {

template<class Entity>
bool indexEntities(const QList<Entity> &entities, QHash<int, const Entity *> &index,
                   const QString &kind, QString &error)
{
    for (const Entity &entity : entities) {
        if (entity.uniqueId < 0)
            continue;
        if (index.contains(entity.uniqueId)) {
            error = QStringLiteral("Duplicate %1 UID %2").arg(kind).arg(entity.uniqueId);
            return false;
        }
        index.insert(entity.uniqueId, &entity);
    }
    return true;
}

template<class Entity>
bool matchEntities(const QList<Entity> &live, const QList<Entity> &incoming,
                   const QSet<int> &reused, QSet<int> &matches, QString &error,
                   const QString &kind)
{
    QHash<int, const Entity *> liveIndex;
    QHash<int, const Entity *> incomingIndex;
    if (!indexEntities(live, liveIndex, kind, error)
        || !indexEntities(incoming, incomingIndex, kind, error))
        return false;

    for (auto it = incomingIndex.constBegin(); it != incomingIndex.constEnd(); ++it) {
        if (liveIndex.contains(it.key()) && !reused.contains(it.key()))
            matches.insert(it.key());
    }
    return true;
}

bool sameEndpoint(int live, int incoming, const QSet<int> &matches)
{
    return live == incoming && (live < 0 || matches.contains(live));
}

} // namespace

bool matchVaultImport(const Project &live, const Project &incoming,
                      const QSet<int> &reusedTasks, const QSet<int> &reusedResources,
                      const QSet<int> &reusedAssignments,
                      VaultImportMatches &matches, QString &error)
{
    matches = {};
    error.clear();
    VaultImportMatches result;
    QHash<int, const Assignment *> liveAssignments;
    QHash<int, const Assignment *> incomingAssignments;

    if (!matchEntities(live.tasks, incoming.tasks, reusedTasks, result.tasks,
                       error, QStringLiteral("task"))
        || !matchEntities(live.resources, incoming.resources, reusedResources, result.resources,
                          error, QStringLiteral("resource"))
        || !indexEntities(live.assignments, liveAssignments, QStringLiteral("assignment"), error)
        || !indexEntities(incoming.assignments, incomingAssignments, QStringLiteral("assignment"), error))
        return false;

    for (auto it = incomingAssignments.constBegin(); it != incomingAssignments.constEnd(); ++it) {
        const Assignment *previous = liveAssignments.value(it.key());
        if (!previous)
            continue;

        const Assignment &candidate = *it.value();
        if (!reusedAssignments.contains(it.key())
            && sameEndpoint(previous->taskUniqueId, candidate.taskUniqueId, result.tasks)
            && sameEndpoint(previous->resourceUniqueId, candidate.resourceUniqueId, result.resources)) {
            result.assignments.insert(it.key());
        } else {
            result.replacedAssignments.insert(it.key());
        }
    }

    matches = result;
    return true;
}

} // namespace schedule
