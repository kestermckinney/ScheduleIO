// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/customfield.h"

namespace schedule {

bool CustomField::operator==(const CustomField &o) const
{
    return fieldId == o.fieldId
        && name == o.name
        && value == o.value
        && formula == o.formula
        && lookupValues == o.lookupValues
        && lookupDescriptions == o.lookupDescriptions
        && restrictToList == o.restrictToList
        && appendNewValues == o.appendNewValues
        && listOrder == o.listOrder
        && defaultValue == o.defaultValue
        && graphicalIndicators == o.graphicalIndicators
        && summaryCalc == o.summaryCalc;
}

} // namespace schedule
