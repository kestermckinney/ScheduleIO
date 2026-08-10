// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_WORKCONTOURING_H
#define SCHEDULE_WORKCONTOURING_H

#include "scheduleio_export.h"
#include "model/project.h"

#include <QString>
#include <QVector>

namespace schedule {

class SCHEDULEIO_EXPORT WorkContouring
{
public:
    enum Contour {
        Flat = 0, BackLoaded = 1, FrontLoaded = 2, DoublePeak = 3,
        EarlyPeak = 4, LatePeak = 5, Bell = 6, Turtle = 7,
        Contoured = 8
    };

    static QString name(int contour);
    static QVector<int> segmentPercentages(int contour);

    // Set the assignment contour and rebuild its remaining-work buckets. The
    // aggregate remaining work and any actual-work buckets are preserved.
    static bool apply(Project &project, int assignmentUid, int contour);

    // Rebuild an existing predefined contour after work or dates change.
    static bool regenerate(Project &project, int assignmentUid);
};

} // namespace schedule

#endif // SCHEDULE_WORKCONTOURING_H
