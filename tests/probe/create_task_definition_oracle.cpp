// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// Generate a deterministic native MPP used by Schedule Vault's remote Project
// oracle for split and recurring task interoperability.
#include "mppio.h"
#include "model/duration.h"

#include <cstdio>

namespace {
constexpr qint64 kHour = 60LL * 60LL * 1000LL;

schedule::Task occurrence(int uid, int id, const QString &name,
                          const QDateTime &start)
{
    schedule::Task task;
    task.uniqueId = uid;
    task.id = id;
    task.outlineLevel = 2;
    task.name = name;
    task.start = start;
    task.finish = start.addSecs(8 * 3600);
    task.durationMillis = 8 * kHour;
    task.durationFormat = schedule::Duration::Days;
    return task;
}
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: create_task_definition_oracle <out.mpp>\n");
        return 2;
    }
    schedule::Project project;
    project.formatVersion = schedule::Project::FormatVersion::Mpp14;
    project.title = QStringLiteral("Split and Recurring Task Oracle");
    project.startDate = QDateTime(QDate(2026, 8, 3), QTime(8, 0));

    schedule::Task recurring;
    recurring.uniqueId = 1;
    recurring.id = 1;
    recurring.outlineLevel = 1;
    recurring.name = QStringLiteral("Weekly inspection");
    recurring.summary = true;
    recurring.recurring = true;
    recurring.start = project.startDate;
    recurring.finish = QDateTime(QDate(2026, 8, 17), QTime(17, 0));
    recurring.durationMillis = 11 * 8 * kHour;
    project.tasks << recurring
                  << occurrence(2, 2, QStringLiteral("Weekly inspection 1"), project.startDate)
                  << occurrence(3, 3, QStringLiteral("Weekly inspection 2"), project.startDate.addDays(7))
                  << occurrence(4, 4, QStringLiteral("Weekly inspection 3"), project.startDate.addDays(14));

    schedule::Task split;
    split.uniqueId = 5;
    split.id = 5;
    split.outlineLevel = 1;
    split.name = QStringLiteral("Split installation");
    split.start = QDateTime(QDate(2026, 8, 4), QTime(8, 0));
    split.finish = QDateTime(QDate(2026, 8, 7), QTime(12, 0));
    split.durationMillis = 16 * kHour;
    split.durationFormat = schedule::Duration::Days;
    split.workMillis = 16 * kHour;
    split.segments = {
        { split.start, QDateTime(QDate(2026, 8, 4), QTime(12, 0)) },
        { QDateTime(QDate(2026, 8, 6), QTime(8, 0)), split.finish }
    };
    project.tasks << split;

    schedule::Resource resource;
    resource.uniqueId = 1;
    resource.id = 1;
    resource.name = QStringLiteral("Installer");
    project.resources << resource;
    schedule::Assignment assignment;
    assignment.uniqueId = 1;
    assignment.taskUniqueId = split.uniqueId;
    assignment.resourceUniqueId = resource.uniqueId;
    assignment.start = split.start;
    assignment.finish = split.finish;
    assignment.workMillis = split.workMillis;
    assignment.remainingWorkMillis = split.workMillis;
    assignment.workContour = 8;
    schedule::TimephasedValue first;
    first.type = schedule::TimephasedValue::RemainingWork;
    first.uniqueId = assignment.uniqueId;
    first.start = split.segments.first().start;
    first.finish = split.segments.first().finish;
    first.unit = 1;
    first.value = QStringLiteral("PT4H0M0S");
    schedule::TimephasedValue second = first;
    second.start = split.segments.last().start;
    second.finish = split.segments.last().finish;
    second.value = QStringLiteral("PT12H0M0S");
    assignment.timephasedValues = { first, second };
    project.assignments << assignment;

    MppIO io;
    io.setProject(project);
    if (!io.save(QString::fromLocal8Bit(argv[1]))) {
        std::fprintf(stderr, "save failed: %s\n", qPrintable(io.errorString()));
        return 1;
    }
    return 0;
}
