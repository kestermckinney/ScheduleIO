// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MPPTASK_H
#define MPPTASK_H

#include "mppio_export.h"

#include <QDateTime>
#include <QString>

// A single task row. Value type: copyable and equality-comparable so that
// round-trip tests can assert model1 == model2 after read->write->read.
class MPPIO_EXPORT MppTask
{
public:
    int uniqueId = 0;       // stable identity across edits (Task UID)
    int id = 0;             // display/outline position
    int outlineLevel = 1;
    QString name;
    QDateTime start;
    QDateTime finish;
    qint64 durationMillis = 0;   // duration normalised to milliseconds
    double percentComplete = 0.0;
    bool milestone = false;
    bool summary = false;

    bool operator==(const MppTask &o) const;
    bool operator!=(const MppTask &o) const { return !(*this == o); }
};

#endif // MPPTASK_H
