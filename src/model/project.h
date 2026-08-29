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
#include "model/timelineviewsettings.h"
#include "model/usageviewsettings.h"
#include "model/viewstyles.h"

#include <QDateTime>
#include <QByteArray>
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
    bool scheduleFromStart = true;   // false = calculate backward from finishDate
    bool multipleCriticalPaths = false; // anchor every independent network at its own finish
    QDateTime statusDate;   // "as of" date for progress / earned-value calculations
    int calendarUniqueId = -1;   // the project calendar; -1 = the "Standard" calendar
    double budgetCost = 0.0;
    qint64 budgetWorkMillis = 0;

    QList<Task> tasks;
    QList<Resource> resources;
    QList<Assignment> assignments;
    QList<Calendar> calendars;
    QList<Relation> relations;
    QList<CustomField> customFieldDefinitions; // formula/lookup/indicator metadata

    // Opaque MPP14 FONT_BASES payload ("   214/Props", key 0x03400000).
    // It is intentionally not part of semantic equality; it preserves the
    // source file's exact font-index mapping when the binary writer uses its
    // stock container template.
    QByteArray mppFontBases;

    // Opaque MPP14 BAR_EXCEPTION_STYLES payload (the Gantt view Props9 item
    // 574619661): MS Project's per-task bar formatting, 71 bytes a task. Only
    // the middle-bar colour is modelled (Task::barColor); the rest -- shapes,
    // patterns, start/end ends, bar text -- is preserved verbatim through a
    // save so hand-formatting done in Project survives a round trip. Opaque and
    // excluded from semantic equality, like mppFontBases.
    QByteArray mppBarExceptions;

    // Original MPP14 container used as the presentation template for an edited
    // save.  Retaining the source lets the writer preserve native view and
    // table rowsets from storage 214 that the semantic model does not decode
    // while regenerating backend records from its stock template.
    // Like mppFontBases, this is opaque and intentionally excluded from
    // semantic equality.
    QByteArray mppSourceTemplate;

    // View formatting template (text styles, gridline/date-line and bar colours),
    // mirroring the Gantt Chart view's stored properties. See ViewStyles.
    ViewStyles viewStyles;

    // Separate style template for the Resource Usage view (text styles + the
    // overallocation highlight). Falls back to viewStyles when not present.
    ViewStyles resourceUsageStyles;

    // Native Microsoft Project Gantt/Usage-view table columns and pane geometry.
    // ScheduleVault-only geometry is intentionally not stored here.
    UsageViewSettings ganttView;
    UsageViewSettings resourceUsageView;
    UsageViewSettings taskUsageView;
    UsageViewSettings teamPlannerView;

    // Separate style template for the Team Planner view (text styles + bar
    // colours). Falls back to viewStyles when not present.
    ViewStyles teamPlannerStyles;

    // Separate style template for the Calendar view (text styles + bar colours).
    // Falls back to viewStyles when not present.
    ViewStyles calendarStyles;

    // Decoded Microsoft Project Timeline view (the <TLViewData> XML document in
    // storage 214). Like the UsageViewSettings members above, this is not part
    // of Project::operator==; the writer re-emits it when timelineView.modified.
    TimelineViewSettings timelineView;

    // User-chosen accent colour for the report charts' primary series (0xRRGGBB), or
    // TextStyle::kAutomatic to keep the built-in blue. Persisted via XML/scaffold; MPP
    // persistence is a known gap.
    qint32 reportAccentColor = TextStyle::kAutomatic;

    bool operator==(const Project &o) const;
    bool operator!=(const Project &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_PROJECT_H
