// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/mpprelation.h"

bool MppRelation::operator==(const MppRelation &o) const
{
    return uniqueId == o.uniqueId
        && predecessorTaskUid == o.predecessorTaskUid
        && successorTaskUid == o.successorTaskUid
        && type == o.type
        && lagMillis == o.lagMillis;
}
