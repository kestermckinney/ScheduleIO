// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#include "projectmetadata.h"
#include "model/project.h"

#include <QDataStream>
#include <QIODevice>

namespace schedule {
namespace {

constexpr quint32 metadataRevision = 1;

// Field order is part of the on-disk contract for revision 1.
template<class P, class Field>
void visitMetadata(P &project, Field field)
{
    field(project.title);
    field(project.author);
    field(project.startDate);
    field(project.finishDate);
    field(project.scheduleFromStart);
    field(project.multipleCriticalPaths);
    field(project.statusDate);
    field(project.calendarUniqueId);
    field(project.budgetCost);
    field(project.budgetWorkMillis);
    field(project.mppFontBases);
    field(project.mppBarExceptions);
    field(project.mppSourceTemplate);
    field(project.reportAccentColor);
    field(project.newTasksManual);
    field(project.newTaskStartIsProjectStart);
    field(project.defaultTaskType);
    field(project.defaultDurationUnits);
    field(project.defaultWorkUnits);
    field(project.newTasksEffortDriven);
    field(project.autoLinkTasks);
    field(project.splitInProgressTasks);
    field(project.honorConstraints);
    field(project.criticalSlackLimit);
    field(project.weekStartDay);
    field(project.fiscalYearStartMonth);
    field(project.fiscalYearUsesStartYear);
    field(project.defaultStartTime);
    field(project.defaultEndTime);
    field(project.minutesPerDay);
    field(project.minutesPerWeek);
    field(project.daysPerMonth);
    field(project.moveCompletedEndsBack);
    field(project.moveRemainingStartsBack);
    field(project.moveRemainingStartsForward);
    field(project.moveCompletedEndsForward);
    field(project.statusUpdatesResource);
    field(project.currencySymbol);
    field(project.currencySymbolPosition);
    field(project.currencyDigits);
    field(project.currencyCode);
    field(project.defaultStandardRate);
    field(project.defaultOvertimeRate);
    field(project.defaultFixedCostAccrual);
    field(project.defaultEarnedValueMethod);
    field(project.baselineForEarnedValue);
    field(project.showProjectSummaryTask);
}

} // namespace

QByteArray encodeVaultProjectMetadata(const Project &project)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_5_15);
    stream << metadataRevision << qint32(project.formatVersion);
    visitMetadata(project, [&](const auto &value) { stream << value; });
    return bytes;
}

bool decodeVaultProjectMetadata(const QByteArray &bytes, Project &project)
{
    QDataStream stream(bytes);
    stream.setVersion(QDataStream::Qt_5_15);
    quint32 revision = 0;
    qint32 format = 0;
    stream >> revision >> format;
    if (revision != metadataRevision)
        return false;

    Project decoded;
    visitMetadata(decoded, [&](auto &value) { stream >> value; });
    if (stream.status() != QDataStream::Ok || !stream.atEnd())
        return false;

    // Replay validated data into the caller's project, preserving collections
    // and presentation structures restored by their own codecs.
    stream.device()->seek(sizeof(revision) + sizeof(format));
    visitMetadata(project, [&](auto &value) { stream >> value; });
    project.formatVersion = static_cast<Project::FormatVersion>(format);
    return true;
}

} // namespace schedule
