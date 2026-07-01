// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_CUSTOMFIELD_H
#define SCHEDULE_CUSTOMFIELD_H

#include "scheduleio_export.h"

#include <QString>
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
    int fieldId = 0;      // full MPP field id (high word entity + low word index),
                          // e.g. 0x0B400073 == task Cost1; matches the XML <FieldID>
    QString name;         // human label, e.g. "Text1", "Cost2", "Outline Code1"
    QVariant value;       // decoded value in its natural type

    bool operator==(const CustomField &o) const;
    bool operator!=(const CustomField &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_CUSTOMFIELD_H
