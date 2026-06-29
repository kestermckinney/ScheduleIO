// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/mppbaseline.h"

bool MppBaseline::operator==(const MppBaseline &o) const
{
    return number == o.number
        && cost == o.cost
        && workMillis == o.workMillis
        && start == o.start
        && finish == o.finish
        && durationMillis == o.durationMillis;
}
