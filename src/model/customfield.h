// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_CUSTOMFIELD_H
#define SCHEDULE_CUSTOMFIELD_H

#include "scheduleio_export.h"

#include <QString>
#include <QStringList>
#include <QList>
#include <QVariant>

namespace schedule {

// One custom ("extended") field value on an entity, e.g. Text1, Number3, Cost2,
// Flag5, Date1, Duration4, Start2 or Outline Code1. Microsoft Project exposes a
// large, fixed set of such slots per entity; rather than ~150 explicit members we
// keep the populated ones in a list of these. `value` holds the decoded value in
// its natural Qt type (QString / double / qint64 / bool / QDateTime) so callers
// switch on `value.typeId()` or on the well-known `name`.
class SCHEDULEIO_EXPORT CustomField
{
public:
    struct IndicatorRule {
        QString comparison; // eq, ne, lt, le, gt, ge, contains
        QVariant value;
        QString indicator;  // symbolic icon name used by modern UI/reports
        bool operator==(const IndicatorRule &o) const
        { return comparison == o.comparison && value == o.value && indicator == o.indicator; }
    };
    int fieldId = 0;      // full MPP field id (high word entity + low word index),
                          // e.g. 0x0B400073 == task Cost1; matches the XML <FieldID>
    QString name;         // human label, e.g. "Text1", "Cost2", "Outline Code1"
    QVariant value;       // decoded value in its natural type
    QString formula;      // Schedule Vault/Project-style expression, e.g. [Cost] / [Duration]
    QStringList lookupValues;
    // Value list options, as Project's Edit Lookup Table dialog has them:
    // a description per value (parallel to lookupValues, may be shorter),
    // whether only listed values may be entered, whether a new value typed
    // into the field joins the list, the order the list is shown in
    // (PjListOrder: 0 as entered, 1 ascending, 2 descending) and the value new
    // tasks get (empty for none).
    QStringList lookupDescriptions;
    bool restrictToList = true;
    bool appendNewValues = false;
    int listOrder = 0;
    QString defaultValue;
    // Indicator rules compare with a value, or with another field when the
    // value is a [Field Name] reference.
    QList<IndicatorRule> graphicalIndicators;
    // Calculation for task and group summary rows, Project's PjSummaryCalc:
    // 0 maximum (Or for a flag), 1 minimum (And), 2 count all, 3 sum,
    // 4 average, 5 average first sublevel, 6 count first sublevel,
    // 7 count non-summaries, 10 none, 11 use the formula.
    int summaryCalc = 10;

    bool operator==(const CustomField &o) const;
    bool operator!=(const CustomField &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_CUSTOMFIELD_H
