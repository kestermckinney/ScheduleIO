// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MPPPROJECT_H
#define MPPPROJECT_H

#include "mppio_export.h"

#include "model/mppassignment.h"
#include "model/mppcalendar.h"
#include "model/mpprelation.h"
#include "model/mppresource.h"
#include "model/mpptask.h"

#include <QDateTime>
#include <QList>
#include <QString>

// The in-memory project document: the Qt data structure callers manipulate.
// Mirrors the WINPROJ "Bknd" object model at a high level (tasks, resources,
// assignments, calendars). A pure value type so models can be compared.
class MPPIO_EXPORT MppProject
{
public:
    // Binary .mpp format families seen in WINPROJ (ProgIDs MSProject.MPP.12 / .14).
    enum class FormatVersion {
        Unknown = 0,
        Mpp12 = 12,   // Project 2007
        Mpp14 = 14,   // Project 2010+
    };

    FormatVersion formatVersion = FormatVersion::Unknown;

    QString title;
    QString author;
    QDateTime startDate;
    QDateTime finishDate;

    QList<MppTask> tasks;
    QList<MppResource> resources;
    QList<MppAssignment> assignments;
    QList<MppCalendar> calendars;
    QList<MppRelation> relations;

    bool operator==(const MppProject &o) const;
    bool operator!=(const MppProject &o) const { return !(*this == o); }
};

#endif // MPPPROJECT_H
