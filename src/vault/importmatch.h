// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#ifndef SCHEDULE_VAULT_IMPORTMATCH_H
#define SCHEDULE_VAULT_IMPORTMATCH_H

#include "scheduleio_export.h"

#include <QSet>
#include <QString>

namespace schedule {
class Project;

struct VaultImportMatches
{
    QSet<int> tasks;
    QSet<int> resources;
    QSet<int> assignments;
    QSet<int> replacedAssignments;
};

// Compare schedules after confirming their project lineage. The caller can
// exclude UIDs identified as reused during review. Matching sets contain UIDs
// whose Vault identities can carry over to the incoming schedule.
SCHEDULEIO_EXPORT bool matchVaultImport(
    const Project &live, const Project &incoming,
    const QSet<int> &reusedTasks, const QSet<int> &reusedResources,
    const QSet<int> &reusedAssignments, VaultImportMatches &matches, QString &error);
}

#endif
