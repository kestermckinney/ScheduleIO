// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_TIMELINEVIEWSETTINGS_H
#define SCHEDULE_TIMELINEVIEWSETTINGS_H

#include "scheduleio_export.h"

#include <QByteArray>
#include <QDate>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace schedule {

// Microsoft Project stores the Timeline view as a self-describing UTF-16 XML
// document, "<TLViewData>", held identically in two places inside `   214`:
//   * the CV_iew Var2Data record of type 47, keyed by the timeline view's uid;
//   * Props9 item key 574619695 (0x2240000F) of that view's type-6 PROPERTIES.
// The structs below are the decoded model. `rawXml` keeps the exact bytes as
// read so the writer can splice the modelled changes into the original document
// and leave everything it does not model (shape/fill <fmt> records, the sentinel
// template rows, unrecognised <options> attributes, ...) byte-for-byte intact.
//
// Reverse-engineered 2026-08-27 from `tests/fixtures/mpp_samples/tl_*` (see
// memory reference-mpp-timeline-tlviewdata). Not modelled yet: the callout /
// text-only display style, and the "shown in the Gantt split" flag (which lives
// in the CV_iew FixedData record, not this XML).

// One timeline bar -- a <tl> element in <tlbarSet>.
//   id 0  is Project's internal/hidden default bar (always emitted in full).
//   id 1+ are the visible bars; members point at them by `barid`.
struct SCHEDULEIO_EXPORT TimelineBar
{
    int id = 0;
    QString label;                 // <tl label="...">, empty when unset
    bool useCustomDates = false;   // <tl useCustomDates="1">
    QDate customStart;             // <tl startDate="YYYY/MM/DD">
    QDate customFinish;            // <tl finishDate="YYYY/MM/DD">

    bool operator==(const TimelineBar &o) const
    {
        return id == o.id && label == o.label && useCustomDates == o.useCustomDates
            && customStart == o.customStart && customFinish == o.customFinish;
    }
    bool operator!=(const TimelineBar &o) const { return !(*this == o); }
};

// How a member is drawn on the timeline. Only Bar round-trips today; Callout /
// TextOnly are kept so the model and UI do not have to change when they are
// decoded (they need UI Automation, not the COM object model, to capture).
enum class TimelineItemDisplay { Bar, Callout, TextOnly };

// One task or milestone on the timeline. MS Project writes every member as BOTH
// a <t> in <tskSet> and an <m> in <mlSet> with the same GUID; this is one row
// for the pair. `milestone` records which set(s) it also belongs to for a
// faithful re-emit.
struct SCHEDULEIO_EXPORT TimelineItem
{
    QString guid;                  // <t id="{...}"> / <m id="{...}">
    int taskUid = -1;              // uid=""
    int barId = 1;                 // barid="" -> the owning TimelineBar::id
    bool onTimeline = true;        // onTL="1"
    bool milestone = false;        // task is a milestone (0-duration)
    TimelineItemDisplay display = TimelineItemDisplay::Bar;

    bool operator==(const TimelineItem &o) const
    {
        return guid == o.guid && taskUid == o.taskUid && barId == o.barId
            && onTimeline == o.onTimeline && milestone == o.milestone
            && display == o.display;
    }
    bool operator!=(const TimelineItem &o) const { return !(*this == o); }
};

// One <style> in <txtSet>: a timeline text category (type 0..11) or an override
// (id 12+). Colour is 0xRRGGBB, or kAutomatic for a theme colour (thm="0001").
struct SCHEDULEIO_EXPORT TimelineTextStyle
{
    static constexpr qint32 kAutomatic = -1;

    int id = 0;
    int type = 0;
    qint32 color = kAutomatic;     // clr="FFRRGGBB" -> 0xRRGGBB; thm != 0 -> kAutomatic
    QString fontName;
    int fontSize = 0;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;

    bool operator==(const TimelineTextStyle &o) const
    {
        return id == o.id && type == o.type && color == o.color
            && fontName == o.fontName && fontSize == o.fontSize && bold == o.bold
            && italic == o.italic && underline == o.underline
            && strikethrough == o.strikethrough;
    }
    bool operator!=(const TimelineTextStyle &o) const { return !(*this == o); }
};

// The decoded Timeline view. Like UsageViewSettings / ViewStyles, `present` and
// `modified` are writer bookkeeping and are NOT part of semantic identity
// (Project::operator== omits this member, matching ganttView / viewStyles-usage).
struct SCHEDULEIO_EXPORT TimelineViewSettings
{
    bool present = false;    // a type-16 view with a <TLViewData> blob was read
    bool modified = false;   // an edit was made; the writer must re-emit the XML

    int viewUid = -1;        // CV_iew FixedData id of the type-16 view record
    QString viewName;        // that record's UTF-16 name
    bool dfltTLView = true;  // <TLViewData dfltTLView="1">

    QList<TimelineBar> bars;             // <tlbarSet>, always includes id 0 after read
    QList<TimelineItem> items;           // <tskSet> + <mlSet>, template rows excluded
    QList<TimelineTextStyle> textStyles; // <txtSet>

    // <options> -- only the attributes Schedule Vault edits are typed; the rest
    // ride along in rawXml. NB the on-disk attribute names for the Today line and
    // the timescale look transposed relative to Project's own enum, so these
    // fields are named by meaning: showTodayLine <-> XML showTS,
    // showTimescale <-> XML showToday (verify when wiring the codec).
    int dateFormat = 255;
    int numTextLines = 1;
    bool showTodayLine = true;
    bool showTimescale = true;
    bool showPanZoom = true;
    bool showDates = true;
    bool showTaskProgress = true;
    bool showOverlaps = true;

    // Exact <TLViewData> bytes as read (UTF-16LE). The writer edits this in place
    // and re-emits it into both storage locations; when the modelled subset is
    // unchanged it is emitted verbatim.
    QByteArray rawXml;

    const TimelineBar *bar(int barId) const
    {
        for (const TimelineBar &b : bars)
            if (b.id == barId)
                return &b;
        return nullptr;
    }
    bool contains(int taskUid) const
    {
        for (const TimelineItem &it : items)
            if (it.taskUid == taskUid)
                return true;
        return false;
    }

    void touch() { present = true; modified = true; }

    // Modelled-fields comparison (not present/modified/rawXml). Used by the
    // writer to decide whether the XML actually needs regenerating.
    bool operator==(const TimelineViewSettings &o) const
    {
        return viewUid == o.viewUid && viewName == o.viewName
            && dfltTLView == o.dfltTLView && bars == o.bars && items == o.items
            && textStyles == o.textStyles && dateFormat == o.dateFormat
            && numTextLines == o.numTextLines && showTodayLine == o.showTodayLine
            && showTimescale == o.showTimescale && showPanZoom == o.showPanZoom
            && showDates == o.showDates && showTaskProgress == o.showTaskProgress
            && showOverlaps == o.showOverlaps;
    }
    bool operator!=(const TimelineViewSettings &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_TIMELINEVIEWSETTINGS_H
