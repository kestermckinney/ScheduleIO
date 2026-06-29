// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MPPRELATION_H
#define MPPRELATION_H

#include "mppio_export.h"

// A task dependency (predecessor link), from the TBkndCons storage. The
// successor task depends on the predecessor task.
class MPPIO_EXPORT MppRelation
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
    qint64 lagMillis = 0;

    bool operator==(const MppRelation &o) const;
    bool operator!=(const MppRelation &o) const { return !(*this == o); }
};

#endif // MPPRELATION_H
