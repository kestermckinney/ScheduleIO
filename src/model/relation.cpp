// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/relation.h"

namespace schedule {

bool Relation::operator==(const Relation &o) const
{
    return uniqueId == o.uniqueId
        && predecessorTaskUid == o.predecessorTaskUid
        && successorTaskUid == o.successorTaskUid
        && type == o.type
        && lagMillis == o.lagMillis
        && lagFormat == o.lagFormat;
}

} // namespace schedule
