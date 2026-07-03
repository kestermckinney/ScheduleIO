// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_TASK_H
#define SCHEDULE_TASK_H

#include "scheduleio_export.h"
#include "model/baseline.h"
#include "model/customfield.h"

#include <QDateTime>
#include <QList>
#include <QString>

namespace schedule {

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
    double percentComplete = 0.0;
    bool milestone = false;
    bool summary = false;
    int constraintType = 0;      // 0 = As Soon As Possible
    QDateTime constraintDate;    // invalid unless the constraint needs a date
    QString wbs;
    QString notes;   // raw RTF source of the task's notes (empty if none)

    // Scheduling behaviour (Task Information dialog fields).
    bool manual = false;         // manually scheduled (vs auto scheduled)
    bool effortDriven = false;   // effort-driven (work fixed as resources change)
    int taskType = 0;            // 0 = Fixed Units, 1 = Fixed Duration, 2 = Fixed Work
    int priority = 500;          // 0..1000, 500 = normal
    QDateTime deadline;          // invalid when no deadline is set
    int calendarUniqueId = -1;   // task calendar; -1 = use the project calendar

    // Critical-path results (Scheduler::computeSlack): the latest dates the
    // task can run without moving the project finish, the slack margins, and
    // whether the task is on the critical path (total slack <= 0).
    QDateTime lateStart;
    QDateTime lateFinish;
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
    double actualCost = 0.0;
    double remainingCost = 0.0;
    double costVariance = 0.0;

    // Saved baselines (number 0 = current baseline, 1..10 = saved), present only
    // when the file stores them. Custom ("extended") field values that are set.
    QList<Baseline> baselines;
    QList<CustomField> customFields;

    bool operator==(const Task &o) const;
    bool operator!=(const Task &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_TASK_H
