// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_PROJECT_H
#define SCHEDULE_PROJECT_H

#include "scheduleio_export.h"

#include "model/assignment.h"
#include "model/calendar.h"
#include "model/relation.h"
#include "model/resource.h"
#include "model/task.h"
#include "model/viewstyles.h"

#include <QDateTime>
#include <QList>
#include <QString>

namespace schedule {

// The in-memory project document: the Qt data structure callers manipulate.
// Mirrors the WINPROJ "Bknd" object model at a high level (tasks, resources,
// assignments, calendars). A pure value type so models can be compared.
class SCHEDULEIO_EXPORT Project
{
public:
    // Binary .mpp format families seen in WINPROJ (ProgIDs MSProject.MPP.12 / .14).
    enum class FormatVersion {
        Unknown = 0,
        Mpp12 = 12,   // Project 2007
        Mpp14 = 14,   // Project 2010+
    };

    FormatVersion formatVersion = FormatVersion::Unknown;

    QString title;
    QString author;
    QDateTime startDate;
    QDateTime finishDate;
    QDateTime statusDate;   // "as of" date for progress / earned-value calculations
    int calendarUniqueId = -1;   // the project calendar; -1 = the "Standard" calendar

    QList<Task> tasks;
    QList<Resource> resources;
    QList<Assignment> assignments;
    QList<Calendar> calendars;
    QList<Relation> relations;

    // View formatting template (text styles, gridline/date-line and bar colours),
    // mirroring the Gantt Chart view's stored properties. See ViewStyles.
    ViewStyles viewStyles;

    // Separate style template for the Resource Usage view (text styles + the
    // overallocation highlight). Falls back to viewStyles when not present.
    ViewStyles resourceUsageStyles;

    // Separate style template for the Team Planner view (text styles + bar
    // colours). Falls back to viewStyles when not present.
    ViewStyles teamPlannerStyles;

    // Separate style template for the Calendar view (text styles + bar colours).
    // Falls back to viewStyles when not present.
    ViewStyles calendarStyles;

    bool operator==(const Project &o) const;
    bool operator!=(const Project &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_PROJECT_H
