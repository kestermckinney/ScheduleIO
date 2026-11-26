// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#ifndef SCHEDULE_VAULT_EXPORTSELECTION_H
#define SCHEDULE_VAULT_EXPORTSELECTION_H

#include "scheduleio_export.h"
#include <QDateTime>
#include <QList>
#include <QSet>
#include <QString>

namespace schedule {
class Project;

// Selects tasks by UID, including descendants of selected summaries and the
// ancestor rows needed to retain the outline. The source project stays intact.
SCHEDULEIO_EXPORT bool selectVaultExportTasks(const Project &source, const QSet<int> &taskUids,
                                              Project &output, QString &error);
struct VaultExportFields
{
    bool baselines = true;
    bool customFields = true;
    bool calendars = true;
};

SCHEDULEIO_EXPORT void applyVaultExportFields(Project &project, const VaultExportFields &fields);
struct VaultApprovedWork
{
    int assignmentUid = -1;
    QDateTime from;
    QDateTime to;
    qint64 workMillis = 0;
    qint64 entryCount = 0;
};

struct VaultActualsExportSummary
{
    int replacedAssignments = 0;
    qint64 omittedEntries = 0;
};

SCHEDULEIO_EXPORT VaultActualsExportSummary applyVaultApprovedWork(
    Project &project, const QList<VaultApprovedWork> &work);
}

#endif
