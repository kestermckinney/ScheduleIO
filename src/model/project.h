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
#include <QTime>

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

    // ---- Project options (MS Project File > Options) --------------------------
    // The subset ScheduleVault's Options dialog edits. All round-trip through
    // MSPDI today; the .mpp side follows once each "   114/Props" key id is
    // reverse-engineered (see src/codec/propskeys.h). Values and defaults mirror
    // MS Project's own (e.g. WeekStartDay is MSPDI's 0=Sunday..6=Saturday, not
    // Qt's 1=Monday..7=Sunday -- callers convert).

    // Schedule
    bool newTasksManual = false;              // NEW_TASKS_ARE_MANUAL
    bool newTaskStartIsProjectStart = true;   // NEW_TASK_START_IS_PROJECT_START (0=start,1=current)
    int defaultTaskType = 0;                  // DEFAULT_TASK_TYPE (0=Fixed Units,1=Fixed Duration,2=Fixed Work)
    int defaultDurationUnits = 7;             // DEFAULT_DURATION_UNITS (MSPDI DurationFormat; 7=days)
    int defaultWorkUnits = 2;                 // DEFAULT_WORK_UNITS (MSPDI WorkFormat; 2=hours)
    bool newTasksEffortDriven = false;        // NEW_TASKS_EFFORT_DRIVEN
    bool autoLinkTasks = true;               // AUTO_LINK (MSPDI Autolink)
    bool splitInProgressTasks = true;        // SPLIT_IN_PROGRESS_TASKS
    bool honorConstraints = false;           // HONOR_CONSTRAINTS
    int criticalSlackLimit = 0;              // CRITICAL_SLACK_LIMIT (whole days)

    // Calendar
    int weekStartDay = 0;                    // WEEK_START_DAY (0=Sunday..6=Saturday)
    int fiscalYearStartMonth = 1;           // FISCAL_YEAR_START_MONTH (MSPDI FYStartDate; 1..12)
    bool fiscalYearUsesStartYear = false;    // MSPDI FiscalYearStart
    QTime defaultStartTime = QTime(8, 0);   // DEFAULT_START_TIME
    QTime defaultEndTime = QTime(17, 0);    // DEFAULT_END_TIME
    int minutesPerDay = 480;                // MINUTES_PER_DAY
    int minutesPerWeek = 2400;              // MINUTES_PER_WEEK
    int daysPerMonth = 20;                  // DAYS_PER_MONTH

    // Calculation options for this project
    bool moveCompletedEndsBack = false;      // MOVE_COMPLETED_ENDS_BACK
    bool moveRemainingStartsBack = false;    // MOVE_REMAINING_STARTS_BACK
    bool moveRemainingStartsForward = false; // MOVE_REMAINING_STARTS_FORWARD
    bool moveCompletedEndsForward = false;   // MOVE_COMPLETED_ENDS_FORWARD
    bool statusUpdatesResource = true;       // UPDATING_TASK_STATUS_UPDATES_RESOURCE_STATUS (MSPDI TaskUpdatesResource)

    // Financial
    QString currencySymbol;                  // CURRENCY_SYMBOL (empty = MS Project's own default)
    int currencySymbolPosition = 0;         // CURRENCY_SYMBOL_POSITION (0=before,1=after,2=before+space,3=after+space)
    int currencyDigits = 2;                 // CURRENCY_DIGITS
    QString currencyCode;                    // CURRENCY_CODE
    double defaultStandardRate = 0.0;        // DEFAULT_STANDARD_RATE
    double defaultOvertimeRate = 0.0;        // DEFAULT_OVERTIME_RATE
    int defaultFixedCostAccrual = 3;        // DEFAULT_FIXED_COST_ACCRUAL (1=Start,2=End,3=Prorated)
    int defaultEarnedValueMethod = 0;      // EARNED_VALUE_METHOD (0=% Complete,1=Physical % Complete)
    int baselineForEarnedValue = 0;         // BASELINE_FOR_EARNED_VALUE (0=Baseline,1..10=Baseline1..10)
    bool showProjectSummaryTask = true;     // SHOW_PROJECT_SUMMARY_TASK

    bool operator==(const Project &o) const;
    bool operator!=(const Project &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_PROJECT_H
