// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MPPTASK_H
#define MPPTASK_H

#include "mppio_export.h"
#include "model/mppbaseline.h"
#include "model/mppcustomfield.h"

#include <QDateTime>
#include <QList>
#include <QString>

// A single task row. Value type: copyable and equality-comparable so that
// round-trip tests can assert model1 == model2 after read->write->read.
class MPPIO_EXPORT MppTask
{
public:
    int uniqueId = 0;       // stable identity across edits (Task UID)
    int id = 0;             // display/outline position
    int outlineLevel = 1;
    QString name;
    QDateTime start;
    QDateTime finish;
    qint64 durationMillis = 0;   // duration normalised to milliseconds
    double percentComplete = 0.0;
    bool milestone = false;
    bool summary = false;
    int constraintType = 0;      // 0 = As Soon As Possible
    QDateTime constraintDate;    // invalid unless the constraint needs a date
    QString wbs;
    QString notes;   // raw RTF source of the task's notes (empty if none)

    // Cost (in the project's currency unit).
    double cost = 0.0;
    double fixedCost = 0.0;
    double actualCost = 0.0;
    double remainingCost = 0.0;
    double costVariance = 0.0;

    // Saved baselines (number 0 = current baseline, 1..10 = saved), present only
    // when the file stores them. Custom ("extended") field values that are set.
    QList<MppBaseline> baselines;
    QList<MppCustomField> customFields;

    bool operator==(const MppTask &o) const;
    bool operator!=(const MppTask &o) const { return !(*this == o); }
};

#endif // MPPTASK_H
