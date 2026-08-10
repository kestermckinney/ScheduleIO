// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_ASSIGNMENT_H
#define SCHEDULE_ASSIGNMENT_H

#include "scheduleio_export.h"
#include "model/baseline.h"
#include "model/customfield.h"
#include "model/timephasedvalue.h"

#include <QDateTime>
#include <QList>

namespace schedule {

// Links a resource to a task (entity type 0xc / "Assignment").
class SCHEDULEIO_EXPORT Assignment
{
public:
    int uniqueId = 0;
    int taskUniqueId = 0;
    int resourceUniqueId = 0;
    bool budget = false;              // project-summary budget assignment
    double budgetCost = 0.0;
    qint64 budgetWorkMillis = 0;
    double units = 1.0;            // assigned units (1.0 == 100%)
    int costRateTable = 0;         // resource cost-rate table A..E (0..4)
    // Material consumption: 0 = fixed quantity; otherwise Microsoft time-unit
    // code (1 minute, 2 hour, 3 day, 4 week, 5 month, 7 year).
    int variableRateUnits = 0;
    // Microsoft Project assignment work contour: 0 Flat, 1 Back Loaded,
    // 2 Front Loaded, 3 Double Peak, 4 Early Peak, 5 Late Peak, 6 Bell,
    // 7 Turtle, 8 Contoured (custom time-phased work).
    int workContour = 0;
    qint64 workMillis = 0;         // assigned work, normalised to milliseconds
    QString notes;                 // raw RTF source of the assignment's notes (empty if none)

    // Scheduled span of the assignment. WINPROJ's invariant is
    //   assignment start == task start + delay + leveling delay
    // with the span lying inside the task's span. Invalid dates mean "same as
    // the task" (common when the file predates these fields being read).
    QDateTime start;
    QDateTime finish;
    QDateTime stop;                  // progress boundary: completed work ends
    QDateTime resume;                // progress boundary: remaining work resumes
    qint64 delayMillis = 0;           // assignment delay, in working time
    qint64 levelingDelayMillis = 0;   // delay added by resource leveling
    qint64 actualWorkMillis = 0;      // work already done
    qint64 remainingWorkMillis = 0;   // work still scheduled
    qint64 overtimeWorkMillis = 0;    // overtime subset of Work
    qint64 actualOvertimeWorkMillis = 0;
    qint64 remainingOvertimeWorkMillis = 0;

    // Cost (in the project's currency unit). Assignments have no fixed cost.
    double cost = 0.0;
    double actualCost = 0.0;
    double remainingCost = 0.0;
    double costVariance = 0.0;
    double overtimeCost = 0.0;
    double actualOvertimeCost = 0.0;
    double remainingOvertimeCost = 0.0;

    QList<Baseline> baselines;        // assignment baselines: cost/work/start/finish
    QList<CustomField> customFields;
    QList<TimephasedValue> timephasedValues;

    // Replace one actual/remaining-work interval while preserving portions of
    // existing buckets outside [from, to). Aggregate work fields are reconciled.
    bool setTimephasedWorkInPeriod(int type, const QDateTime &from,
                                   const QDateTime &to, qint64 millis);
    qint64 timephasedWorkInPeriod(int type, const QDateTime &from,
                                  const QDateTime &to) const;
    bool setTimephasedCostInPeriod(int type, const QDateTime &from,
                                   const QDateTime &to, double amount,
                                   int baselineNumber = 0);
    double timephasedCostInPeriod(int type, const QDateTime &from,
                                  const QDateTime &to, int baselineNumber = 0) const;

    // Microsoft Project serializes time-phased material consumption through
    // the assignment work stream.  One hour in that stream represents one
    // material unit; these helpers keep that storage convention out of UI code.
    bool setTimephasedMaterialInPeriod(int type, const QDateTime &from,
                                       const QDateTime &to, double quantity);
    double timephasedMaterialInPeriod(int type, const QDateTime &from,
                                      const QDateTime &to) const;

    bool operator==(const Assignment &o) const;
    bool operator!=(const Assignment &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_ASSIGNMENT_H
