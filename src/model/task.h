// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_TASK_H
#define SCHEDULE_TASK_H

#include "scheduleio_export.h"
#include "model/baseline.h"
#include "model/customfield.h"
#include "model/viewstyles.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>

namespace schedule {

// One contiguous working portion of a split task. Microsoft Project exposes
// these through Task.SplitParts; an unsplit task leaves this list empty.
struct SCHEDULEIO_EXPORT TaskSegment
{
    QDateTime start;
    QDateTime finish;

    bool operator==(const TaskSegment &o) const
    {
        return start == o.start && finish == o.finish;
    }
};

// Earned-value / PMI performance metrics as stored by Microsoft Project. These are
// the file's own values; consumers may prefer them when present (non-zero) and fall
// back to computing from baseline cost, % complete and actual cost otherwise. BAC
// (baseline cost) and VAC (BAC-EAC) are derived elsewhere, not stored here.
struct SCHEDULEIO_EXPORT EarnedValue
{
    double pv = 0.0;     // Planned Value  (BCWS)
    double ev = 0.0;     // Earned Value   (BCWP)
    double ac = 0.0;     // Actual Cost    (ACWP)
    double cv = 0.0;     // Cost Variance      (EV - AC)
    double sv = 0.0;     // Schedule Variance  (EV - PV)
    double cpi = 0.0;    // Cost Performance Index      (EV / AC)
    double spi = 0.0;    // Schedule Performance Index  (EV / PV)
    double eac = 0.0;    // Estimate At Completion
    double tcpi = 0.0;   // To-Complete Performance Index

    bool operator==(const EarnedValue &o) const;
    bool operator!=(const EarnedValue &o) const { return !(*this == o); }
};

// A single task row. Value type: copyable and equality-comparable so that
// round-trip tests can assert model1 == model2 after read->write->read.
class SCHEDULEIO_EXPORT Task
{
public:
    int uniqueId = 0;       // stable identity across edits (Task UID)
    int id = 0;             // display/outline position
    int outlineLevel = 1;
    QString name;
    QDateTime start;
    QDateTime finish;
    qint64 durationMillis = 0;   // duration normalised to milliseconds
    int durationFormat = 7;      // display unit for the duration (Duration::Unit, 7 = days)
    qint64 workMillis = 0;       // task-level work; holds Work when no resource is
                                 // assigned (MS Project keeps a task Work independent
                                 // of assignments). Mirrors the assignment total when
                                 // resources exist -- see TaskScheduling.
    // Transient (not serialised): the Work value while the task still had no work
    // resources. Staffing the task -- any number of resources, added together or
    // one at a time -- holds this total (duration/units flex to fit), the way MS
    // Project does when resources are assigned in one action. Reset to -1 once the
    // work is assignment-shaped (an assignment's work or units edited) or the task
    // is progressed; a reloaded file starts at -1 and re-adopts the stored Work on
    // the first schedule while it stays resourceless.
    qint64 enteredWorkMillis = -1;
    double percentComplete = 0.0;
    double physicalPercentComplete = 0.0; // 0..1, independent of duration progress
    int earnedValueMethod = 0;            // 0 = % Complete, 1 = Physical % Complete
    bool milestone = false;
    bool summary = false;
    bool recurring = false;       // recurring-task summary/occurrence container
    QList<TaskSegment> segments;  // two or more portions when the task is split
    int constraintType = 0;      // 0 = As Soon As Possible
    QDateTime constraintDate;    // invalid unless the constraint needs a date
    QString wbs;
    QString notes;   // raw RTF source of the task's notes (empty if none)
    QString hyperlink;           // display text (Project Task.Hyperlink)
    QString hyperlinkAddress;    // URL or file address
    QString hyperlinkSubAddress; // optional bookmark/location inside the target

    // Inactive tasks (MS Project Professional's Task > Inactivate) stay in the plan but
    // are excluded from scheduling: they impose no constraints on successors and don't
    // count toward rollups, resource allocation, leveling, cost or earned value. Drawn
    // greyed with strikethrough. `active` defaults true (a normal task).
    bool active = true;

    // Scheduling behaviour (Task Information dialog fields).
    bool manual = false;         // manually scheduled (vs auto scheduled)
    qint64 levelingDelayMillis = 0;   // working-time delay added by resource leveling;
                                      // the forward pass pushes the task start by this
    // Transient (not serialised): the un-levelled start a no-predecessor task is
    // anchored at, so leveling delay is applied to a stable base each reschedule instead
    // of compounding into `start`. Set by ResourceLeveling; invalid means "use start".
    QDateTime levelingAnchor;
    bool effortDriven = false;   // effort-driven (work fixed as resources change)
    int taskType = 0;            // 0 = Fixed Units, 1 = Fixed Duration, 2 = Fixed Work
    int priority = 500;          // 0..1000, 500 = normal
    QDateTime deadline;          // invalid when no deadline is set
    int calendarUniqueId = -1;   // task calendar; -1 = use the project calendar
    bool ignoreResourceCalendar = false; // task calendar alone governs assigned work

    // Critical-path results (Scheduler::computeSlack): the latest dates the
    // task can run without moving the project finish, the slack margins, and
    // whether the task is on the critical path (total slack <= 0).
    QDateTime earlyStart;
    QDateTime earlyFinish;
    QDateTime lateStart;
    QDateTime lateFinish;
    qint64 startSlackMillis = 0;
    qint64 finishSlackMillis = 0;
    qint64 totalSlackMillis = 0;
    qint64 freeSlackMillis = 0;
    bool critical = false;

    // Recorded actuals (invalid/zero until the task has progress).
    QDateTime actualStart;
    QDateTime actualFinish;
    qint64 actualDurationMillis = 0;
    qint64 actualWorkMillis = 0;

    // Earned-value metrics stored in the file (see EarnedValue).
    EarnedValue evm;

    // Cost (in the project's currency unit).
    double cost = 0.0;
    double fixedCost = 0.0;
    // How the task's fixed cost is booked over its span: 1 = Start, 2 = End,
    // 3 = Prorated (MS Project's default). Seeded from Project::defaultFixedCostAccrual
    // on task creation. Round-trips via MSPDI and the scaffold; MPP-binary
    // persistence of the per-task field is a known gap.
    int fixedCostAccrual = 3;
    double actualCost = 0.0;
    double remainingCost = 0.0;
    double costVariance = 0.0;

    // Current schedule/work deltas from Baseline 0. Date variances are signed
    // task-calendar working durations, matching Project's displayed fields.
    qint64 startVarianceMillis = 0;
    qint64 finishVarianceMillis = 0;
    qint64 durationVarianceMillis = 0;
    qint64 workVarianceMillis = 0;

    // Row text formatting (MS Project's Format > Font): emphasis + colours applied
    // to the task's grid row. Stored in the MPP file as the Gantt Chart view's
    // per-cell "exceptional" text styles, including font family/size through the
    // view's FONT_BASES table.
    TextStyle rowFormat;

    // Per-task Gantt bar colour override (MS Project's Format > Bar). kAutomatic (-1)
    // means "use the category bar style". Round-trips via XML/scaffold; MPP-binary
    // persistence of a per-task bar format is a known gap.
    qint32 barColor = TextStyle::kAutomatic;

    // Per-cell (per-column) text/background formatting. Built-ins use ScheduleVault's
    // stable numeric ColType key; custom fields use "c:<field name>". The style overlays
    // `rowFormat` for that one column and persists as an MPP column-property record.
    QHash<QString, TextStyle> cellFormats;

    // Saved baselines (number 0 = current baseline, 1..10 = saved), present only
    // when the file stores them. Custom ("extended") field values that are set.
    QList<Baseline> baselines;
    QList<CustomField> customFields;

    bool operator==(const Task &o) const;
    bool operator!=(const Task &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_TASK_H
