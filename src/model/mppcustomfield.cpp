// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/mppcustomfield.h"

bool MppCustomField::operator==(const MppCustomField &o) const
{
    return fieldId == o.fieldId
        && name == o.name
        && value == o.value;
}
