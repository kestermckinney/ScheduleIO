// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/mppresource.h"

bool MppResource::operator==(const MppResource &o) const
{
    return uniqueId == o.uniqueId
        && id == o.id
        && name == o.name
        && initials == o.initials
        && qFuzzyCompare(maxUnits + 1.0, o.maxUnits + 1.0);
}
