// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_RESOURCE_H
#define SCHEDULE_RESOURCE_H

#include "scheduleio_export.h"
#include "model/baseline.h"
#include "model/costrate.h"
#include "model/customfield.h"

#include <QList>
#include <QString>

namespace schedule {

class SCHEDULEIO_EXPORT Resource
{
public:
    int uniqueId = 0;
    int id = 0;
    QString name;
    QString initials;
    double maxUnits = 1.0;   // 1.0 == 100%
    QString notes;           // raw RTF source of the resource's notes (empty if none)
    int calendarUniqueId = -1;   // the resource's own calendar; -1 = none recorded

    // Cost (in the project's currency unit). Resources have no fixed cost.
    double cost = 0.0;
    double actualCost = 0.0;
    double remainingCost = 0.0;
    double costVariance = 0.0;

    QList<Baseline> baselines;        // resource baselines store cost + work
    QList<CustomField> customFields;
    QList<CostRate> costRates;        // cost-rate tables A..E (time-phased rates)

    bool operator==(const Resource &o) const;
    bool operator!=(const Resource &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_RESOURCE_H
