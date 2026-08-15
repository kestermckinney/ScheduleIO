// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef USAGEVIEWSETTINGS_H
#define USAGEVIEWSETTINGS_H

#include "scheduleio_export.h"

#include <QList>
#include <QString>
#include <QStringList>
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

// Presentation state shared by the Gantt, Task Usage and Resource Usage views.
// Native fields use Project's view records. detailSelection preserves an exact
// application selection when it includes calculated details Project cannot
// represent in its native VIEW_FIELDS collection.
struct SCHEDULEIO_EXPORT UsageViewSettings
{
    bool present = false;
    bool modified = false; // writer patch request; not part of semantic identity
    QString viewName;
    QString tableName;
    QList<UsageTableColumn> columns;
    QList<int> detailFields; // ordered native Usage-view VIEW_FIELDS identifiers
    QStringList detailSelection; // exact Task Usage selection (MPP extension)
    int timescaleSize = 100; // Project's Timescale "Size" / Enlarge percentage
    int tableWidth = 0;      // width of the table (left) pane in a split view

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

    void setDetailFields(const QList<int> &fields)
    {
        detailFields.clear();
        for (int field : fields) {
            if (field >= 0 && field < 255 && !detailFields.contains(field))
                detailFields.append(field);
        }
        present = true;
        modified = true;
    }

    void setDetailSelection(const QStringList &details)
    {
        detailSelection.clear();
        for (const QString &detail : details) {
            if (!detail.isEmpty() && !detailSelection.contains(detail))
                detailSelection.append(detail);
        }
        present = true;
        modified = true;
    }

    void setTableWidth(int width)
    {
        tableWidth = qBound(0, width, 65535);
        present = true;
        modified = true;
    }

    bool operator==(const UsageViewSettings &o) const
    {
        return present == o.present && viewName == o.viewName
            && tableName == o.tableName && columns == o.columns
            && detailFields == o.detailFields && detailSelection == o.detailSelection
            && timescaleSize == o.timescaleSize
            && tableWidth == o.tableWidth;
    }
};

} // namespace schedule

#endif // USAGEVIEWSETTINGS_H
