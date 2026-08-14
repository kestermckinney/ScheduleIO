// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef USAGEVIEWSETTINGS_H
#define USAGEVIEWSETTINGS_H

#include "scheduleio_export.h"

#include <QList>
#include <QString>
#include <QtGlobal>

namespace schedule {

// One native Microsoft Project table column. Width is the byte-sized value
// stored in MPP14 CTable column records (the Project UI exposes the same value
// through its table editing APIs).
struct SCHEDULEIO_EXPORT UsageTableColumn
{
    quint32 fieldId = 0;
    int width = 0;
    QString title;

    bool operator==(const UsageTableColumn &o) const
    {
        return fieldId == o.fieldId && width == o.width && title == o.title;
    }
};

// Native presentation state shared by the Task Usage and Resource Usage views.
// This deliberately contains only Microsoft Project concepts; ScheduleVault-
// specific layout state belongs in the application's QSettings.
struct SCHEDULEIO_EXPORT UsageViewSettings
{
    bool present = false;
    bool modified = false; // writer patch request; not part of semantic identity
    QString viewName;
    QString tableName;
    QList<UsageTableColumn> columns;
    int timescaleSize = 100; // Project's Timescale "Size" / Enlarge percentage

    int columnWidth(quint32 fieldId, int fallback = 0) const
    {
        for (const UsageTableColumn &column : columns)
            if (column.fieldId == fieldId)
                return column.width;
        return fallback;
    }

    void setColumnWidth(quint32 fieldId, int width)
    {
        for (UsageTableColumn &column : columns) {
            if (column.fieldId == fieldId) {
                column.width = qBound(0, width, 255);
                present = true;
                modified = true;
                return;
            }
        }
        UsageTableColumn column;
        column.fieldId = fieldId;
        column.width = qBound(0, width, 255);
        columns.append(column);
        present = true;
        modified = true;
    }

    void setTimescaleSize(int percent)
    {
        timescaleSize = qBound(25, percent, 255);
        present = true;
        modified = true;
    }

    bool operator==(const UsageViewSettings &o) const
    {
        return present == o.present && viewName == o.viewName
            && tableName == o.tableName && columns == o.columns
            && timescaleSize == o.timescaleSize;
    }
};

} // namespace schedule

#endif // USAGEVIEWSETTINGS_H
