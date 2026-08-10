// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/timephasedvalue.h"

#include <QRegularExpression>
#include <cmath>

namespace schedule {

qint64 TimephasedValue::durationMillis() const
{
    static const QRegularExpression re(
        QStringLiteral(R"(^(-)?P(?:(\d+(?:\.\d+)?)D)?(?:T(?:(\d+(?:\.\d+)?)H)?(?:(\d+(?:\.\d+)?)M)?(?:(\d+(?:\.\d+)?)S)?)?$)"));
    const QRegularExpressionMatch m = re.match(value.trimmed());
    if (!m.hasMatch())
        return 0;
    const double days = m.captured(2).toDouble();
    const double hours = m.captured(3).toDouble();
    const double minutes = m.captured(4).toDouble();
    const double seconds = m.captured(5).toDouble();
    qint64 millis = qint64(std::llround(
        (((days * 24.0 + hours) * 60.0 + minutes) * 60.0 + seconds) * 1000.0));
    return m.captured(1).isEmpty() ? millis : -millis;
}

qint64 TimephasedValue::durationInPeriod(const QDateTime &from, const QDateTime &to) const
{
    if (!start.isValid() || !finish.isValid() || finish <= start || to <= from)
        return 0;
    const QDateTime overlapStart = qMax(start, from);
    const QDateTime overlapFinish = qMin(finish, to);
    if (overlapFinish <= overlapStart)
        return 0;
    const qint64 bucketSpan = start.msecsTo(finish);
    if (bucketSpan <= 0)
        return 0;
    return qint64(std::llround(double(durationMillis())
        * double(overlapStart.msecsTo(overlapFinish)) / double(bucketSpan)));
}

double TimephasedValue::amount() const
{
    bool ok = false;
    const double number = value.trimmed().toDouble(&ok);
    return ok ? number : 0.0;
}

double TimephasedValue::amountInPeriod(const QDateTime &from, const QDateTime &to) const
{
    if (!start.isValid() || !finish.isValid() || finish <= start || to <= from)
        return 0.0;
    const QDateTime overlapStart = qMax(start, from);
    const QDateTime overlapFinish = qMin(finish, to);
    if (overlapFinish <= overlapStart) return 0.0;
    return amount() * double(overlapStart.msecsTo(overlapFinish))
        / double(start.msecsTo(finish));
}

bool TimephasedValue::operator==(const TimephasedValue &o) const
{
    return type == o.type
        && uniqueId == o.uniqueId
        && start == o.start
        && finish == o.finish
        && unit == o.unit
        && baselineNumber == o.baselineNumber
        && value == o.value;
}

} // namespace schedule
