// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_RELATION_H
#define SCHEDULE_RELATION_H

#include "scheduleio_export.h"

namespace schedule {

// A task dependency (predecessor link), from the TBkndCons storage. The
// successor task depends on the predecessor task.
class SCHEDULEIO_EXPORT Relation
{
public:
    enum Type {
        FinishToStart = 1,   // most common
        StartToStart = 3,
        FinishToFinish = 0,
        StartToFinish = 2,
    };

    int uniqueId = 0;
    int predecessorTaskUid = 0;
    int successorTaskUid = 0;
    int type = FinishToStart;
    qint64 lagMillis = 0;   // lag (or negative lead) normalised to milliseconds
    int lagFormat = 7;      // display unit for the lag (Duration::Unit, 7 = days)

    bool operator==(const Relation &o) const;
    bool operator!=(const Relation &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_RELATION_H
