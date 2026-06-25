// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MPPRESOURCE_H
#define MPPRESOURCE_H

#include "mppio_export.h"

#include <QString>

class MPPIO_EXPORT MppResource
{
public:
    int uniqueId = 0;
    int id = 0;
    QString name;
    QString initials;
    double maxUnits = 1.0;   // 1.0 == 100%

    bool operator==(const MppResource &o) const;
    bool operator!=(const MppResource &o) const { return !(*this == o); }
};

#endif // MPPRESOURCE_H
