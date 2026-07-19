// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_VIEWSTYLES_H
#define SCHEDULE_VIEWSTYLES_H

#include "scheduleio_export.h"

#include <QString>

namespace schedule {

// One text style: font emphasis + colours for a view text category or a task-row
// override (Format > Font / Format > Text Styles in Microsoft Project). The font
// family/size are deliberately NOT modelled -- Schedule Vault only edits emphasis
// and colours, leaving the file's font bases untouched. Colours are 0xRRGGBB;
// kAutomatic means MS Project's "Automatic" (the flag byte in the file).
class SCHEDULEIO_EXPORT TextStyle
{
public:
    static constexpr qint32 kAutomatic = -1;

    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;
    qint32 color = kAutomatic;       // 0xRRGGBB, or kAutomatic
    qint32 backColor = kAutomatic;   // cell background, or kAutomatic
    int backPattern = 0;             // MPP BackgroundPattern (0 transparent, 1 solid, ...)
    // Font family/size overrides (empty/0 = inherit the view's font base). MS Project's
    // MPP stores fonts via a font-base table; the codec resolves and creates table
    // entries as needed.
    QString fontName;
    int fontSize = 0;                // points; 0 = inherit
    // Exact index into MPP's FONT_BASES table. Kept alongside the friendly
    // fields so an MPP read/write can preserve undocumented font variants
    // byte-for-byte even when two entries share the same family name.
    int fontBaseIndex = -1;

    bool isDefault() const
    {
        return !bold && !italic && !underline && !strikethrough
            && color == kAutomatic && backColor == kAutomatic && backPattern == 0
            && fontName.isEmpty() && fontSize == 0 && fontBaseIndex < 0;
    }

    bool operator==(const TextStyle &o) const
    {
        return bold == o.bold && italic == o.italic && underline == o.underline
            && strikethrough == o.strikethrough && color == o.color
            && backColor == o.backColor && backPattern == o.backPattern
            && fontName == o.fontName && fontSize == o.fontSize;
    }
    bool operator!=(const TextStyle &o) const { return !(*this == o); }
};

// A chart line style (gridlines, the status/current date lines): colour + MPP
// LineStyle (0 none, 1 solid, 2 dotted1, 3 dotted2, 4 dashed).
class SCHEDULEIO_EXPORT ViewLineStyle
{
public:
    qint32 color = TextStyle::kAutomatic;   // 0xRRGGBB, or kAutomatic
    int lineStyle = 0;

    bool operator==(const ViewLineStyle &o) const
    {
        return color == o.color && lineStyle == o.lineStyle;
    }
    bool operator!=(const ViewLineStyle &o) const { return !(*this == o); }
};

// A Gantt bar's editable colours (the bar body and its start/end end-shapes).
class SCHEDULEIO_EXPORT ViewBarStyle
{
public:
    qint32 middleColor = TextStyle::kAutomatic;
    qint32 startColor = TextStyle::kAutomatic;
    qint32 endColor = TextStyle::kAutomatic;

    bool operator==(const ViewBarStyle &o) const
    {
        return middleColor == o.middleColor && startColor == o.startColor
            && endColor == o.endColor;
    }
    bool operator!=(const ViewBarStyle &o) const { return !(*this == o); }
};

// The project's view formatting template: text styles per display category plus
// the editable Gantt chart element styles, mirroring what Microsoft Project
// stores in the Gantt Chart view's property data (CV_iew STYLE_DATA). Applied
// by Schedule Vault to both the Gantt view and the Resource Usage view.
class SCHEDULEIO_EXPORT ViewStyles
{
public:
    // Text categories, in the order their style blocks appear in STYLE_DATA
    // (offsets 26 + 32*n; see MPXJ GanttChartView14.processViewProperties).
    enum TextCategory {
        Highlighted = 0, RowAndColumn, NonCritical, Critical, Summary, Milestone,
        MiddleTimescale, BottomTimescale, BarTextLeft, BarTextRight, BarTextTop,
        BarTextBottom, BarTextInside, Marked, ProjectSummary, External, TopTimescale,
        TextCategoryCount
    };

    // True once styles were loaded from a file or edited; a writer only patches
    // the view data when set.
    bool present = false;

    TextStyle text[TextCategoryCount];

    // Gridlines / date lines of the Gantt chart.
    ViewLineStyle sheetRows;
    ViewLineStyle sheetColumns;
    ViewLineStyle ganttRows;
    ViewLineStyle currentDateLine;
    ViewLineStyle statusDateLine;

    // Bar colours for the standard bar categories (matched by bar-style name in
    // the file: "Task", "Milestone", "Summary", "Project Summary").
    ViewBarStyle taskBar;
    ViewBarStyle milestone;
    ViewBarStyle summaryBar;
    ViewBarStyle projectSummaryBar;

    bool operator==(const ViewStyles &o) const
    {
        for (int i = 0; i < TextCategoryCount; ++i)
            if (text[i] != o.text[i])
                return false;
        return present == o.present
            && sheetRows == o.sheetRows && sheetColumns == o.sheetColumns
            && ganttRows == o.ganttRows && currentDateLine == o.currentDateLine
            && statusDateLine == o.statusDateLine
            && taskBar == o.taskBar && milestone == o.milestone
            && summaryBar == o.summaryBar && projectSummaryBar == o.projectSummaryBar;
    }
    bool operator!=(const ViewStyles &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_VIEWSTYLES_H
