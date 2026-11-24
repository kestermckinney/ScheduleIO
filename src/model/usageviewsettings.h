// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef USAGEVIEWSETTINGS_H
#define USAGEVIEWSETTINGS_H

#include "scheduleio_export.h"
#include "model/viewautofilter.h"

#include <QByteArray>
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

// The definition Microsoft Project keeps in a view for the group `name`
// (Project 2016's own bytes): "No Group", the built-in task groups "Critical",
// "Status", "Complete and Incomplete Tasks" and "Auto Scheduled v. Manually
// Scheduled" (also spelled "...vs. ..."), the built-in resource group
// "Resource Type", and "Outline Level" (a custom group on the Outline Level
// field). Empty for any other name.
SCHEDULEIO_EXPORT QByteArray projectGroupDefinition(const QString &name);

// Presentation state shared by the Gantt, Task Usage and Resource Usage views.
// Native fields use Project's view records. detailSelection preserves an exact
// application selection when it includes calculated details Project cannot
// represent in its native VIEW_FIELDS collection.
struct SCHEDULEIO_EXPORT UsageViewSettings
{
    bool present = false;
    bool modified = false; // writer patch request; not part of semantic identity
    // Writer request: `columns` is the table's complete ordered column list
    // (rebuild the native table), not just width overrides for its columns.
    bool columnLayout = false;
    QString viewName;
    QString tableName;
    QList<UsageTableColumn> columns;
    QList<int> detailFields; // ordered native Usage-view VIEW_FIELDS identifiers
    QStringList detailSelection; // exact Task Usage selection (MPP extension)
    int timescaleSize = 100; // Project's Timescale "Size" / Enlarge percentage
    int tableWidth = 0;      // width of the table (left) pane in a split view
    // The view's column AutoFilters (Project ANDs them). Written whenever the
    // view is modified, so clearing the list clears the file's filters.
    QList<ColumnAutoFilter> autoFilters;
    // The named filter the view has applied, spelled as Project stores it
    // ("&All Tasks", "Critical"); empty when the file's view records none.
    // Written whenever it differs from the file's, ignoring the '&' hotkey.
    QString filterName;
    // The group the view has applied, as Project names it ("No Group",
    // "Critical"), and the copy of its definition Project keeps in the view
    // (the 83-byte Props9 item 0x2240001D). Empty when the view records none.
    // Written whenever the name differs from the file's.
    QString groupName;
    QByteArray groupDefinition;

    void setFilterName(const QString &name)
    {
        filterName = name;
        present = true;
    }

    // Apply one of Project's groups (see projectGroupDefinition). False, and
    // nothing changed, for a group whose definition ScheduleIO doesn't carry.
    bool setGroup(const QString &name);

    void setAutoFilters(const QList<ColumnAutoFilter> &filters)
    {
        autoFilters = filters;
        present = true;
        modified = true;
    }

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

    // Replace the table's visible columns (in order). Entries with fieldId 0
    // are dropped; widths are Project's character widths (0 keeps the stored
    // width of a field already in the table).
    void setColumns(const QList<UsageTableColumn> &list)
    {
        QList<UsageTableColumn> next;
        for (UsageTableColumn column : list) {
            if (column.fieldId == 0)
                continue;
            column.width = qBound(0, column.width, 255);
            next.append(column);
        }
        if (next.isEmpty())
            return;
        columns = next;
        present = true;
        modified = true;
        columnLayout = true;
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
            && tableWidth == o.tableWidth && autoFilters == o.autoFilters
            && filterName == o.filterName && groupName == o.groupName
            && groupDefinition == o.groupDefinition;
    }
};

} // namespace schedule

#endif // USAGEVIEWSETTINGS_H
