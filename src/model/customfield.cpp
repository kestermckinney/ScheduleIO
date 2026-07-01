// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/customfield.h"

namespace schedule {

bool CustomField::operator==(const CustomField &o) const
{
    return fieldId == o.fieldId
        && name == o.name
        && value == o.value;
}

} // namespace schedule
