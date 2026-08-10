// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_TIMEPHASEDVALUE_H
#define SCHEDULE_TIMEPHASEDVALUE_H

#include "scheduleio_export.h"

#include <QDateTime>
#include <QString>

namespace schedule {

// One MSPDI TimephasedData bucket. Value remains in its original XML form so
// unknown bucket types can round-trip without losing precision or semantics.
class SCHEDULEIO_EXPORT TimephasedValue
{
public:
    enum AssignmentType {
        RemainingWork = 1,
        ActualWork = 2,
        ActualOvertimeWork = 3,
        BaselineWork = 4,
        BaselineCost = 5,
        ActualCost = 6
    };

    int type = 0;
    int uniqueId = 0;
    QDateTime start;
    QDateTime finish;
    int unit = 0;
    int baselineNumber = 0;
    QString value;

    qint64 durationMillis() const;
    qint64 durationInPeriod(const QDateTime &from, const QDateTime &to) const;
    double amount() const;
    double amountInPeriod(const QDateTime &from, const QDateTime &to) const;

    bool operator==(const TimephasedValue &o) const;
    bool operator!=(const TimephasedValue &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_TIMEPHASEDVALUE_H
