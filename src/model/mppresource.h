// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MPPRESOURCE_H
#define MPPRESOURCE_H

#include "mppio_export.h"
#include "model/mppbaseline.h"
#include "model/mppcostrate.h"
#include "model/mppcustomfield.h"

#include <QList>
#include <QString>

class MPPIO_EXPORT MppResource
{
public:
    int uniqueId = 0;
    int id = 0;
    QString name;
    QString initials;
    double maxUnits = 1.0;   // 1.0 == 100%
    QString notes;           // raw RTF source of the resource's notes (empty if none)

    // Cost (in the project's currency unit). Resources have no fixed cost.
    double cost = 0.0;
    double actualCost = 0.0;
    double remainingCost = 0.0;
    double costVariance = 0.0;

    QList<MppBaseline> baselines;        // resource baselines store cost + work
    QList<MppCustomField> customFields;
    QList<MppCostRate> costRates;        // cost-rate tables A..E (time-phased rates)

    bool operator==(const MppResource &o) const;
    bool operator!=(const MppResource &o) const { return !(*this == o); }
};

#endif // MPPRESOURCE_H
