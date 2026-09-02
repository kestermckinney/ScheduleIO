// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_VIEWSTYLES_H
#define SCHEDULE_VIEWSTYLES_H

#include "scheduleio_export.h"

#include <QString>
#include <QVector>

#include <array>

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
        // fontName / fontSize / fontBaseIndex are intentionally excluded: the
        // family and size are "deliberately not modelled" (see the class note and
        // tst_format_oracle) -- the writer normalises them and the reader
        // re-derives them from the font-base table, so semantic equality must not
        // hinge on a value neither side round-trips. Emphasis and colours are the
        // modelled, comparable state.
        return bold == o.bold && italic == o.italic && underline == o.underline
            && strikethrough == o.strikethrough && color == o.color
            && backColor == o.backColor && backPattern == o.backPattern;
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

// One row of the Gantt Chart view's default bar-style table (Format > Bar
// Styles): a 195-byte STYLE_DATA record. Enum-ish fields hold the *raw* MPP byte
// / field-id values -- the codec is the file's shape and Schedule Vault's
// GanttChartFormat does the friendly-enum translation. See DECODING_NOTES.md
// "Default bar styles (STYLE_DATA)" for the offsets. A zero / -1 field means
// "unset / inherit", matching MS Project's own encoding.
class SCHEDULEIO_EXPORT ViewBarStyle
{
public:
    // The three editable colours (0xRRGGBB or kAutomatic). Kept first and under
    // their historical names so the { mid, start, end } aggregate initialiser and
    // the colour-only call sites keep working.
    qint32 middleColor = TextStyle::kAutomatic;   // record +2
    qint32 startColor = TextStyle::kAutomatic;    // record +16
    qint32 endColor = TextStyle::kAutomatic;      // record +29

    // The file's style name (record +91). This is the key MS Project and the
    // codec match rows by, so it is part of a row's identity.
    QString name;

    quint8 middleShape = 0;      // +0   MPP GanttBarMiddleShape byte
    quint8 middlePattern = 0;    // +1   MPP pattern byte
    quint8 middleFlag = 0;       // +14  undecoded; preserved verbatim
    quint8 startShape = 0;       // +15  packed: shape = v % 25, type = v / 25
    quint8 endShape = 0;         // +28  packed, as startShape
    qint32 fromField = 0;        // +41  raw 0x0B40---- field id (0 = unset)
    qint32 toField = 0;          // +45  raw field id
    quint64 showFor = 0;         // +49  "show for these" criteria bitset
    quint64 showForNot = 0;      // +57  "but not for these" criteria bitset
    int row = 1;                 // +65  1..4 (file stores 0-based)
    // Bar-text field per position (Left, Right, Top, Bottom, Inside): a
    // FIELD_ARRAY index, or -1 for none. Records +67, +71, +75, +79, +83.
    std::array<qint32, 5> barText { -1, -1, -1, -1, -1 };
    quint16 styleId = 0;         // +89
    quint16 flag87 = 0;          // +87  undecoded; preserved verbatim

    bool operator==(const ViewBarStyle &o) const
    {
        return middleColor == o.middleColor && startColor == o.startColor
            && endColor == o.endColor && name == o.name
            && middleShape == o.middleShape && middlePattern == o.middlePattern
            && middleFlag == o.middleFlag && startShape == o.startShape
            && endShape == o.endShape && fromField == o.fromField
            && toField == o.toField && showFor == o.showFor
            && showForNot == o.showForNot && row == o.row && barText == o.barText
            && styleId == o.styleId && flag87 == o.flag87;
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

    // The Gantt Chart view's default bar-style table, in file order. Rows are
    // matched by name. Empty until styles are read from a file or edited.
    QVector<ViewBarStyle> barStyles;

    // The row for a given style name, creating it if absent (non-const) or
    // returning a shared empty style (const). Named accessors follow for the
    // four categories whose colours Schedule Vault has always edited.
    ViewBarStyle &bar(const QString &name)
    {
        for (ViewBarStyle &b : barStyles)
            if (b.name.compare(name, Qt::CaseInsensitive) == 0)
                return b;
        ViewBarStyle created;
        created.name = name;
        barStyles.append(created);
        return barStyles.last();
    }
    const ViewBarStyle &bar(const QString &name) const
    {
        for (const ViewBarStyle &b : barStyles)
            if (b.name.compare(name, Qt::CaseInsensitive) == 0)
                return b;
        static const ViewBarStyle kEmpty;
        return kEmpty;
    }

    ViewBarStyle &taskBar() { return bar(QStringLiteral("Task")); }
    ViewBarStyle &milestone() { return bar(QStringLiteral("Milestone")); }
    ViewBarStyle &summaryBar() { return bar(QStringLiteral("Summary")); }
    ViewBarStyle &projectSummaryBar() { return bar(QStringLiteral("Project Summary")); }
    const ViewBarStyle &taskBar() const { return bar(QStringLiteral("Task")); }
    const ViewBarStyle &milestone() const { return bar(QStringLiteral("Milestone")); }
    const ViewBarStyle &summaryBar() const { return bar(QStringLiteral("Summary")); }
    const ViewBarStyle &projectSummaryBar() const { return bar(QStringLiteral("Project Summary")); }

    bool operator==(const ViewStyles &o) const
    {
        for (int i = 0; i < TextCategoryCount; ++i)
            if (text[i] != o.text[i])
                return false;
        return present == o.present
            && sheetRows == o.sheetRows && sheetColumns == o.sheetColumns
            && ganttRows == o.ganttRows && currentDateLine == o.currentDateLine
            && statusDateLine == o.statusDateLine
            && barStyles == o.barStyles;
    }
    bool operator!=(const ViewStyles &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_VIEWSTYLES_H
