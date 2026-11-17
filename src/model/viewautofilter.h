// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef VIEWAUTOFILTER_H
#define VIEWAUTOFILTER_H

#include "scheduleio_export.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace schedule {

// One column AutoFilter of a Microsoft Project view (the funnel in a column
// heading), as Project stores it in the view's properties: the column's field,
// which of Project's AutoFilter choices made it (PjAutoFilterType), and the
// criteria Project evaluates -- one or two conditions joined by And / Or, or a
// list of values (PjAutoFilterType In).
struct SCHEDULEIO_EXPORT AutoFilterValue
{
    // How the value is typed in the file. Duration and Work are millisecond
    // amounts (durationUnit: Project's duration unit code, 7 = days); Date is a
    // local date-time; Percent, Number and Cost are numbers; Flag is 0 / 1.
    enum class Type { Text, Number, Cost, Percent, Duration, Work, Date, Flag };

    Type type = Type::Text;
    QString text;
    double number = 0.0;
    qint64 millis = 0;
    int durationUnit = 7;
    QDateTime date;

    bool operator==(const AutoFilterValue &o) const
    {
        return type == o.type && text == o.text && qFuzzyCompare(number + 1.0, o.number + 1.0)
            && millis == o.millis && durationUnit == o.durationUnit && date == o.date;
    }
};

struct SCHEDULEIO_EXPORT AutoFilterCondition
{
    // Project's comparison codes in filter criteria.
    enum Test : quint16 {
        Within = 0x3E8, Greater = 0x3E9, Less = 0x3EA, GreaterEq = 0x3EB, LessEq = 0x3EC,
        Equals = 0x3ED, NotEquals = 0x3EE, Contains = 0x3EF, NotWithin = 0x3F0,
        NotContains = 0x3F1, InList = 0x3F3,
    };

    quint16 test = Equals;
    // One value; two for Within / NotWithin (the lower bound first).
    QList<AutoFilterValue> values;

    bool operator==(const AutoFilterCondition &o) const
    {
        return test == o.test && values == o.values;
    }
};

struct SCHEDULEIO_EXPORT ColumnAutoFilter
{
    // PjAutoFilterType values Project records with the criteria.
    enum Type { Custom = 1, In = 2 };

    quint32 fieldId = 0;      // 0x0B40xxxx task / 0x0C40xxxx resource field
    int filterType = Custom;  // PjAutoFilterType
    bool orJoin = false;      // two conditions: Or instead of And
    QList<AutoFilterCondition> conditions;   // 1 or 2 (not for In)
    QStringList list;         // In: the ticked values

    bool operator==(const ColumnAutoFilter &o) const
    {
        return fieldId == o.fieldId && filterType == o.filterType && orJoin == o.orJoin
            && conditions == o.conditions && list == o.list;
    }
};

} // namespace schedule

#endif // VIEWAUTOFILTER_H
