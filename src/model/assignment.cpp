// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/assignment.h"

#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <utility>

namespace schedule {

namespace {

constexpr qint64 kMaterialUnitMillis = 3600LL * 1000LL;

QString isoDuration(qint64 millis)
{
    quint64 seconds = quint64(qMax<qint64>(0, millis)) / 1000;
    const quint64 hours = seconds / 3600;
    seconds %= 3600;
    const quint64 minutes = seconds / 60;
    seconds %= 60;
    return QStringLiteral("PT%1H%2M%3S").arg(hours).arg(minutes).arg(seconds);
}

} // namespace

double Assignment::timephasedMaterialInPeriod(int type, const QDateTime &from,
                                               const QDateTime &to) const
{
    return double(timephasedWorkInPeriod(type, from, to))
        / double(kMaterialUnitMillis);
}

bool Assignment::setTimephasedMaterialInPeriod(int type, const QDateTime &from,
                                                const QDateTime &to,
                                                double quantity)
{
    if (!std::isfinite(quantity) || quantity < 0.0)
        return false;
    return setTimephasedWorkInPeriod(
        type, from, to,
        qint64(std::llround(quantity * double(kMaterialUnitMillis))));
}

qint64 Assignment::timephasedWorkInPeriod(int type, const QDateTime &from,
                                          const QDateTime &to) const
{
    qint64 total = 0;
    for (const TimephasedValue &value : timephasedValues)
        if (value.type == type)
            total += value.durationInPeriod(from, to);
    return total;
}

bool Assignment::setTimephasedWorkInPeriod(int type, const QDateTime &from,
                                           const QDateTime &to, qint64 millis)
{
    if ((type != TimephasedValue::RemainingWork
         && type != TimephasedValue::ActualWork
         && type != TimephasedValue::ActualOvertimeWork)
        || !from.isValid() || !to.isValid() || to <= from || millis < 0)
        return false;

    QList<TimephasedValue> edited;
    edited.reserve(timephasedValues.size() + 2);
    for (const TimephasedValue &value : std::as_const(timephasedValues)) {
        if (value.type != type || value.finish <= from || to <= value.start) {
            edited.append(value);
            continue;
        }
        if (value.start < from) {
            TimephasedValue left = value;
            left.finish = from;
            left.value = isoDuration(value.durationInPeriod(value.start, from));
            if (left.durationMillis() > 0)
                edited.append(left);
        }
        if (to < value.finish) {
            TimephasedValue right = value;
            right.start = to;
            right.value = isoDuration(value.durationInPeriod(to, value.finish));
            if (right.durationMillis() > 0)
                edited.append(right);
        }
    }

    if (millis > 0) {
        TimephasedValue value;
        value.type = type;
        value.uniqueId = uniqueId;
        value.start = from;
        value.finish = to;
        value.unit = 1;
        value.value = isoDuration(millis);
        edited.append(value);
    }
    std::sort(edited.begin(), edited.end(), [](const auto &left, const auto &right) {
        if (left.start != right.start)
            return left.start < right.start;
        return left.type < right.type;
    });
    timephasedValues = edited;

    qint64 typedTotal = 0;
    QDateTime first;
    QDateTime last;
    for (const TimephasedValue &value : std::as_const(timephasedValues)) {
        if (value.type != type)
            continue;
        typedTotal += qMax<qint64>(0, value.durationMillis());
        if (!first.isValid() || value.start < first)
            first = value.start;
        if (!last.isValid() || value.finish > last)
            last = value.finish;
    }
    if (type == TimephasedValue::ActualWork) {
        actualWorkMillis = typedTotal;
        stop = last;
    } else if (type == TimephasedValue::RemainingWork) {
        remainingWorkMillis = typedTotal;
        resume = first;
    } else {
        actualOvertimeWorkMillis = typedTotal;
        overtimeWorkMillis = actualOvertimeWorkMillis + remainingOvertimeWorkMillis;
        return true;
    }
    workMillis = actualWorkMillis + remainingWorkMillis;
    return true;
}

bool Assignment::setTimephasedCostInPeriod(int type, const QDateTime &from,
                                            const QDateTime &to, double amount,
                                            int baselineNumber)
{
    if ((type != TimephasedValue::ActualCost && type != TimephasedValue::BaselineCost)
        || !from.isValid() || !to.isValid() || to <= from || amount < 0.0)
        return false;
    QList<TimephasedValue> edited;
    for (const TimephasedValue &value : std::as_const(timephasedValues)) {
        if (value.type != type || value.baselineNumber != baselineNumber
            || value.finish <= from || value.start >= to) {
            edited.append(value); continue;
        }
        if (value.start < from) {
            TimephasedValue left = value; left.finish = from;
            left.value = QString::number(value.amountInPeriod(value.start, from), 'g', 15);
            edited.append(left);
        }
        if (value.finish > to) {
            TimephasedValue right = value; right.start = to;
            right.value = QString::number(value.amountInPeriod(to, value.finish), 'g', 15);
            edited.append(right);
        }
    }
    if (amount > 0.0) {
        TimephasedValue value; value.type = type; value.uniqueId = uniqueId;
        value.start = from; value.finish = to; value.unit = 2;
        value.baselineNumber = baselineNumber;
        value.value = QString::number(amount, 'g', 15); edited.append(value);
    }
    timephasedValues = edited;
    if (type == TimephasedValue::ActualCost) {
        actualCost = timephasedCostInPeriod(type, QDateTime(QDate(1900,1,1),QTime(0,0)),
                                            QDateTime(QDate(2200,1,1),QTime(0,0)));
        cost = actualCost + remainingCost;
    }
    return true;
}

double Assignment::timephasedCostInPeriod(int type, const QDateTime &from,
                                           const QDateTime &to, int baselineNumber) const
{
    double total = 0.0;
    for (const TimephasedValue &value : timephasedValues)
        if (value.type == type && value.baselineNumber == baselineNumber)
            total += value.amountInPeriod(from, to);
    return total;
}

bool Assignment::operator==(const Assignment &o) const
{
    return uniqueId == o.uniqueId
        && taskUniqueId == o.taskUniqueId
        && resourceUniqueId == o.resourceUniqueId
        && budget == o.budget
        && budgetCost == o.budgetCost
        && budgetWorkMillis == o.budgetWorkMillis
        && qFuzzyCompare(units + 1.0, o.units + 1.0)
        && costRateTable == o.costRateTable
        && variableRateUnits == o.variableRateUnits
        && workContour == o.workContour
        && workMillis == o.workMillis
        && notes == o.notes
        && start == o.start
        && finish == o.finish
        && stop == o.stop
        && resume == o.resume
        && delayMillis == o.delayMillis
        && levelingDelayMillis == o.levelingDelayMillis
        && actualWorkMillis == o.actualWorkMillis
        && remainingWorkMillis == o.remainingWorkMillis
        && overtimeWorkMillis == o.overtimeWorkMillis
        && actualOvertimeWorkMillis == o.actualOvertimeWorkMillis
        && remainingOvertimeWorkMillis == o.remainingOvertimeWorkMillis
        && cost == o.cost
        && actualCost == o.actualCost
        && remainingCost == o.remainingCost
        && costVariance == o.costVariance
        && overtimeCost == o.overtimeCost
        && actualOvertimeCost == o.actualOvertimeCost
        && remainingOvertimeCost == o.remainingOvertimeCost
        && baselines == o.baselines
        && customFields == o.customFields
        && timephasedValues == o.timephasedValues;
}

} // namespace schedule
