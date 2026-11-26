// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#include "schedulecodec.h"
#include "model/project.h"
#include <QDataStream>
#include <QIODevice>
#include <utility>

namespace schedule {

// Keep these overloads local to this translation unit so the file codecs
// and unrelated Qt streams retain their existing behavior.
static QDataStream &operator<<(QDataStream &, const Baseline &);
static QDataStream &operator>>(QDataStream &, Baseline &);
static QDataStream &operator<<(QDataStream &, const AvailabilityPeriod &);
static QDataStream &operator>>(QDataStream &, AvailabilityPeriod &);
static QDataStream &operator<<(QDataStream &, const CostRate &);
static QDataStream &operator>>(QDataStream &, CostRate &);
static QDataStream &operator<<(QDataStream &, const TimephasedValue &);
static QDataStream &operator>>(QDataStream &, TimephasedValue &);
static QDataStream &operator<<(QDataStream &, const CustomField &);
static QDataStream &operator>>(QDataStream &, CustomField &);
static QDataStream &operator<<(QDataStream &, const TextStyle &);
static QDataStream &operator>>(QDataStream &, TextStyle &);
static QDataStream &operator<<(QDataStream &, const ViewLineStyle &);
static QDataStream &operator>>(QDataStream &, ViewLineStyle &);
static QDataStream &operator<<(QDataStream &, const ViewBarStyle &);
static QDataStream &operator>>(QDataStream &, ViewBarStyle &);
static QDataStream &operator<<(QDataStream &, const ViewStyles &);
static QDataStream &operator>>(QDataStream &, ViewStyles &);
static QDataStream &operator<<(QDataStream &, const AutoFilterValue &);
static QDataStream &operator>>(QDataStream &, AutoFilterValue &);
static QDataStream &operator<<(QDataStream &, const AutoFilterCondition &);
static QDataStream &operator>>(QDataStream &, AutoFilterCondition &);
static QDataStream &operator<<(QDataStream &, const ColumnAutoFilter &);
static QDataStream &operator>>(QDataStream &, ColumnAutoFilter &);
static QDataStream &operator<<(QDataStream &, const UsageTableColumn &);
static QDataStream &operator>>(QDataStream &, UsageTableColumn &);
static QDataStream &operator<<(QDataStream &, const UsageViewSettings &);
static QDataStream &operator>>(QDataStream &, UsageViewSettings &);
static QDataStream &operator<<(QDataStream &, const TimelineBar &);
static QDataStream &operator>>(QDataStream &, TimelineBar &);
static QDataStream &operator<<(QDataStream &, const TimelineItem &);
static QDataStream &operator>>(QDataStream &, TimelineItem &);
static QDataStream &operator<<(QDataStream &, const TimelineTextStyle &);
static QDataStream &operator>>(QDataStream &, TimelineTextStyle &);
static QDataStream &operator<<(QDataStream &, const TimelineViewSettings &);
static QDataStream &operator>>(QDataStream &, TimelineViewSettings &);
static QDataStream &operator<<(QDataStream &, const TimeRange &);
static QDataStream &operator>>(QDataStream &, TimeRange &);
static QDataStream &operator<<(QDataStream &, const CalendarException &);
static QDataStream &operator>>(QDataStream &, CalendarException &);
static QDataStream &operator<<(QDataStream &, const Calendar &);
static QDataStream &operator>>(QDataStream &, Calendar &);
static QDataStream &operator<<(QDataStream &, const Relation &);
static QDataStream &operator>>(QDataStream &, Relation &);
static QDataStream &operator<<(QDataStream &, const TaskSegment &);
static QDataStream &operator>>(QDataStream &, TaskSegment &);
static QDataStream &operator<<(QDataStream &, const EarnedValue &);
static QDataStream &operator>>(QDataStream &, EarnedValue &);
static QDataStream &operator<<(QDataStream &, const Task &);
static QDataStream &operator>>(QDataStream &, Task &);
static QDataStream &operator<<(QDataStream &, const Resource &);
static QDataStream &operator>>(QDataStream &, Resource &);
static QDataStream &operator<<(QDataStream &, const Assignment &);
static QDataStream &operator>>(QDataStream &, Assignment &);
static QDataStream &operator<<(QDataStream &, const CustomField::IndicatorRule &);
static QDataStream &operator>>(QDataStream &, CustomField::IndicatorRule &);
static QDataStream &operator<<(QDataStream &, const Project &);
static QDataStream &operator>>(QDataStream &, Project &);

template<class Stream, class T, size_t N>
void writeArray(Stream &stream, const T (&items)[N])
{
    for (const auto &item : items)
        stream << item;
}

template<class Stream, class T, size_t N>
void readArray(Stream &stream, T (&items)[N])
{
    for (auto &item : items)
        stream >> item;
}

static QDataStream &operator<<(QDataStream &stream, const Baseline &record)
{
    stream << record.number;
    stream << record.cost;
    stream << record.workMillis;
    stream << record.start;
    stream << record.finish;
    stream << record.durationMillis;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, Baseline &record)
{
    stream >> record.number;
    stream >> record.cost;
    stream >> record.workMillis;
    stream >> record.start;
    stream >> record.finish;
    stream >> record.durationMillis;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const AvailabilityPeriod &record)
{
    stream << record.startDate;
    stream << record.endDate;
    stream << record.units;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, AvailabilityPeriod &record)
{
    stream >> record.startDate;
    stream >> record.endDate;
    stream >> record.units;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const CostRate &record)
{
    stream << record.table;
    stream << record.startDate;
    stream << record.endDate;
    stream << record.standardRate;
    stream << record.standardRateUnit;
    stream << record.overtimeRate;
    stream << record.overtimeRateUnit;
    stream << record.costPerUse;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, CostRate &record)
{
    stream >> record.table;
    stream >> record.startDate;
    stream >> record.endDate;
    stream >> record.standardRate;
    stream >> record.standardRateUnit;
    stream >> record.overtimeRate;
    stream >> record.overtimeRateUnit;
    stream >> record.costPerUse;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const TimephasedValue &record)
{
    stream << record.type;
    stream << record.uniqueId;
    stream << record.start;
    stream << record.finish;
    stream << record.unit;
    stream << record.baselineNumber;
    stream << record.value;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, TimephasedValue &record)
{
    stream >> record.type;
    stream >> record.uniqueId;
    stream >> record.start;
    stream >> record.finish;
    stream >> record.unit;
    stream >> record.baselineNumber;
    stream >> record.value;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const CustomField &record)
{
    stream << record.fieldId;
    stream << record.name;
    stream << record.value;
    stream << record.formula;
    stream << record.lookupValues;
    stream << record.lookupDescriptions;
    stream << record.restrictToList;
    stream << record.appendNewValues;
    stream << record.listOrder;
    stream << record.defaultValue;
    stream << record.graphicalIndicators;
    stream << record.summaryCalc;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, CustomField &record)
{
    stream >> record.fieldId;
    stream >> record.name;
    stream >> record.value;
    stream >> record.formula;
    stream >> record.lookupValues;
    stream >> record.lookupDescriptions;
    stream >> record.restrictToList;
    stream >> record.appendNewValues;
    stream >> record.listOrder;
    stream >> record.defaultValue;
    stream >> record.graphicalIndicators;
    stream >> record.summaryCalc;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const TextStyle &record)
{
    stream << record.bold;
    stream << record.italic;
    stream << record.underline;
    stream << record.strikethrough;
    stream << record.color;
    stream << record.backColor;
    stream << record.backPattern;
    stream << record.fontName;
    stream << record.fontSize;
    stream << record.fontBaseIndex;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, TextStyle &record)
{
    stream >> record.bold;
    stream >> record.italic;
    stream >> record.underline;
    stream >> record.strikethrough;
    stream >> record.color;
    stream >> record.backColor;
    stream >> record.backPattern;
    stream >> record.fontName;
    stream >> record.fontSize;
    stream >> record.fontBaseIndex;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const ViewLineStyle &record)
{
    stream << record.color;
    stream << record.lineStyle;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, ViewLineStyle &record)
{
    stream >> record.color;
    stream >> record.lineStyle;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const ViewBarStyle &record)
{
    stream << record.middleColor;
    stream << record.startColor;
    stream << record.endColor;
    stream << record.name;
    stream << record.middleShape;
    stream << record.middlePattern;
    stream << record.middleFlag;
    stream << record.startShape;
    stream << record.endShape;
    stream << record.fromField;
    stream << record.toField;
    stream << record.showFor;
    stream << record.showForNot;
    stream << record.row;
    for (const auto &item : record.barText)
        stream << item;
    stream << record.styleId;
    stream << record.flag87;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, ViewBarStyle &record)
{
    stream >> record.middleColor;
    stream >> record.startColor;
    stream >> record.endColor;
    stream >> record.name;
    stream >> record.middleShape;
    stream >> record.middlePattern;
    stream >> record.middleFlag;
    stream >> record.startShape;
    stream >> record.endShape;
    stream >> record.fromField;
    stream >> record.toField;
    stream >> record.showFor;
    stream >> record.showForNot;
    stream >> record.row;
    for (auto &item : record.barText)
        stream >> item;
    stream >> record.styleId;
    stream >> record.flag87;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const ViewStyles &record)
{
    stream << record.present;
    writeArray(stream, record.text);
    stream << record.sheetRows;
    stream << record.sheetColumns;
    stream << record.ganttRows;
    stream << record.currentDateLine;
    stream << record.statusDateLine;
    stream << record.barStyles;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, ViewStyles &record)
{
    stream >> record.present;
    readArray(stream, record.text);
    stream >> record.sheetRows;
    stream >> record.sheetColumns;
    stream >> record.ganttRows;
    stream >> record.currentDateLine;
    stream >> record.statusDateLine;
    stream >> record.barStyles;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const AutoFilterValue &record)
{
    stream << record.type;
    stream << record.text;
    stream << record.number;
    stream << record.millis;
    stream << record.durationUnit;
    stream << record.date;
    stream << record.anyTime;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, AutoFilterValue &record)
{
    stream >> record.type;
    stream >> record.text;
    stream >> record.number;
    stream >> record.millis;
    stream >> record.durationUnit;
    stream >> record.date;
    stream >> record.anyTime;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const AutoFilterCondition &record)
{
    stream << record.test;
    stream << record.values;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, AutoFilterCondition &record)
{
    stream >> record.test;
    stream >> record.values;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const ColumnAutoFilter &record)
{
    stream << record.fieldId;
    stream << record.filterType;
    stream << record.orJoin;
    stream << record.conditions;
    stream << record.list;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, ColumnAutoFilter &record)
{
    stream >> record.fieldId;
    stream >> record.filterType;
    stream >> record.orJoin;
    stream >> record.conditions;
    stream >> record.list;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const UsageTableColumn &record)
{
    stream << record.fieldId;
    stream << record.width;
    stream << record.title;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, UsageTableColumn &record)
{
    stream >> record.fieldId;
    stream >> record.width;
    stream >> record.title;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const UsageViewSettings &record)
{
    stream << record.present;
    stream << record.columnLayout;
    stream << record.viewName;
    stream << record.tableName;
    stream << record.columns;
    stream << record.detailFields;
    stream << record.detailSelection;
    stream << record.timescaleSize;
    stream << record.tableWidth;
    stream << record.autoFilters;
    stream << record.filterName;
    stream << record.groupName;
    stream << record.groupDefinition;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, UsageViewSettings &record)
{
    stream >> record.present;
    stream >> record.columnLayout;
    stream >> record.viewName;
    stream >> record.tableName;
    stream >> record.columns;
    stream >> record.detailFields;
    stream >> record.detailSelection;
    stream >> record.timescaleSize;
    stream >> record.tableWidth;
    stream >> record.autoFilters;
    stream >> record.filterName;
    stream >> record.groupName;
    stream >> record.groupDefinition;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const TimelineBar &record)
{
    stream << record.id;
    stream << record.label;
    stream << record.useCustomDates;
    stream << record.customStart;
    stream << record.customFinish;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, TimelineBar &record)
{
    stream >> record.id;
    stream >> record.label;
    stream >> record.useCustomDates;
    stream >> record.customStart;
    stream >> record.customFinish;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const TimelineItem &record)
{
    stream << record.guid;
    stream << record.taskUid;
    stream << record.barId;
    stream << record.onTimeline;
    stream << record.milestone;
    stream << record.display;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, TimelineItem &record)
{
    stream >> record.guid;
    stream >> record.taskUid;
    stream >> record.barId;
    stream >> record.onTimeline;
    stream >> record.milestone;
    stream >> record.display;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const TimelineTextStyle &record)
{
    stream << record.id;
    stream << record.type;
    stream << record.color;
    stream << record.fontName;
    stream << record.fontSize;
    stream << record.bold;
    stream << record.italic;
    stream << record.underline;
    stream << record.strikethrough;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, TimelineTextStyle &record)
{
    stream >> record.id;
    stream >> record.type;
    stream >> record.color;
    stream >> record.fontName;
    stream >> record.fontSize;
    stream >> record.bold;
    stream >> record.italic;
    stream >> record.underline;
    stream >> record.strikethrough;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const TimelineViewSettings &record)
{
    stream << record.present;
    stream << record.viewUid;
    stream << record.viewName;
    stream << record.dfltTLView;
    stream << record.bars;
    stream << record.items;
    stream << record.textStyles;
    stream << record.dateFormat;
    stream << record.numTextLines;
    stream << record.showTodayLine;
    stream << record.showTimescale;
    stream << record.showPanZoom;
    stream << record.showDates;
    stream << record.showTaskProgress;
    stream << record.showOverlaps;
    stream << record.rawXml;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, TimelineViewSettings &record)
{
    stream >> record.present;
    stream >> record.viewUid;
    stream >> record.viewName;
    stream >> record.dfltTLView;
    stream >> record.bars;
    stream >> record.items;
    stream >> record.textStyles;
    stream >> record.dateFormat;
    stream >> record.numTextLines;
    stream >> record.showTodayLine;
    stream >> record.showTimescale;
    stream >> record.showPanZoom;
    stream >> record.showDates;
    stream >> record.showTaskProgress;
    stream >> record.showOverlaps;
    stream >> record.rawXml;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const TimeRange &record)
{
    stream << record.start;
    stream << record.end;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, TimeRange &record)
{
    stream >> record.start;
    stream >> record.end;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const CalendarException &record)
{
    stream << record.fromDate;
    stream << record.toDate;
    stream << record.name;
    stream << record.working;
    stream << record.workingTimes;
    stream << record.recurrence;
    stream << record.interval;
    stream << record.weekDayMask;
    stream << record.dayOfMonth;
    stream << record.month;
    stream << record.weekPosition;
    stream << record.occurrences;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, CalendarException &record)
{
    stream >> record.fromDate;
    stream >> record.toDate;
    stream >> record.name;
    stream >> record.working;
    stream >> record.workingTimes;
    stream >> record.recurrence;
    stream >> record.interval;
    stream >> record.weekDayMask;
    stream >> record.dayOfMonth;
    stream >> record.month;
    stream >> record.weekPosition;
    stream >> record.occurrences;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const Calendar &record)
{
    stream << record.uniqueId;
    stream << record.name;
    stream << record.baseCalendarUniqueId;
    stream << record.workingDayMask;
    stream << record.workingTimes;
    stream << record.exceptions;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, Calendar &record)
{
    stream >> record.uniqueId;
    stream >> record.name;
    stream >> record.baseCalendarUniqueId;
    stream >> record.workingDayMask;
    stream >> record.workingTimes;
    stream >> record.exceptions;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const Relation &record)
{
    stream << record.uniqueId;
    stream << record.predecessorTaskUid;
    stream << record.successorTaskUid;
    stream << record.type;
    stream << record.lagMillis;
    stream << record.lagFormat;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, Relation &record)
{
    stream >> record.uniqueId;
    stream >> record.predecessorTaskUid;
    stream >> record.successorTaskUid;
    stream >> record.type;
    stream >> record.lagMillis;
    stream >> record.lagFormat;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const TaskSegment &record)
{
    stream << record.start;
    stream << record.finish;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, TaskSegment &record)
{
    stream >> record.start;
    stream >> record.finish;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const EarnedValue &record)
{
    stream << record.pv;
    stream << record.ev;
    stream << record.ac;
    stream << record.cv;
    stream << record.sv;
    stream << record.cpi;
    stream << record.spi;
    stream << record.eac;
    stream << record.tcpi;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, EarnedValue &record)
{
    stream >> record.pv;
    stream >> record.ev;
    stream >> record.ac;
    stream >> record.cv;
    stream >> record.sv;
    stream >> record.cpi;
    stream >> record.spi;
    stream >> record.eac;
    stream >> record.tcpi;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const Task &record)
{
    stream << record.uniqueId;
    stream << record.id;
    stream << record.outlineLevel;
    stream << record.name;
    stream << record.start;
    stream << record.finish;
    stream << record.durationMillis;
    stream << record.durationFormat;
    stream << record.workMillis;
    stream << record.enteredWorkMillis;
    stream << record.percentComplete;
    stream << record.physicalPercentComplete;
    stream << record.earnedValueMethod;
    stream << record.milestone;
    stream << record.summary;
    stream << record.recurring;
    stream << record.segments;
    stream << record.constraintType;
    stream << record.constraintDate;
    stream << record.wbs;
    stream << record.notes;
    stream << record.hyperlink;
    stream << record.hyperlinkAddress;
    stream << record.hyperlinkSubAddress;
    stream << record.active;
    stream << record.manual;
    stream << record.levelingDelayMillis;
    stream << record.effortDriven;
    stream << record.taskType;
    stream << record.priority;
    stream << record.deadline;
    stream << record.calendarUniqueId;
    stream << record.ignoreResourceCalendar;
    stream << record.earlyStart;
    stream << record.earlyFinish;
    stream << record.lateStart;
    stream << record.lateFinish;
    stream << record.startSlackMillis;
    stream << record.finishSlackMillis;
    stream << record.totalSlackMillis;
    stream << record.freeSlackMillis;
    stream << record.critical;
    stream << record.actualStart;
    stream << record.actualFinish;
    stream << record.actualDurationMillis;
    stream << record.actualWorkMillis;
    stream << record.evm;
    stream << record.cost;
    stream << record.fixedCost;
    stream << record.fixedCostAccrual;
    stream << record.actualCost;
    stream << record.remainingCost;
    stream << record.costVariance;
    stream << record.startVarianceMillis;
    stream << record.finishVarianceMillis;
    stream << record.durationVarianceMillis;
    stream << record.workVarianceMillis;
    stream << record.rowFormat;
    stream << record.barColor;
    stream << record.cellFormats;
    stream << record.baselines;
    stream << record.customFields;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, Task &record)
{
    stream >> record.uniqueId;
    stream >> record.id;
    stream >> record.outlineLevel;
    stream >> record.name;
    stream >> record.start;
    stream >> record.finish;
    stream >> record.durationMillis;
    stream >> record.durationFormat;
    stream >> record.workMillis;
    stream >> record.enteredWorkMillis;
    stream >> record.percentComplete;
    stream >> record.physicalPercentComplete;
    stream >> record.earnedValueMethod;
    stream >> record.milestone;
    stream >> record.summary;
    stream >> record.recurring;
    stream >> record.segments;
    stream >> record.constraintType;
    stream >> record.constraintDate;
    stream >> record.wbs;
    stream >> record.notes;
    stream >> record.hyperlink;
    stream >> record.hyperlinkAddress;
    stream >> record.hyperlinkSubAddress;
    stream >> record.active;
    stream >> record.manual;
    stream >> record.levelingDelayMillis;
    stream >> record.effortDriven;
    stream >> record.taskType;
    stream >> record.priority;
    stream >> record.deadline;
    stream >> record.calendarUniqueId;
    stream >> record.ignoreResourceCalendar;
    stream >> record.earlyStart;
    stream >> record.earlyFinish;
    stream >> record.lateStart;
    stream >> record.lateFinish;
    stream >> record.startSlackMillis;
    stream >> record.finishSlackMillis;
    stream >> record.totalSlackMillis;
    stream >> record.freeSlackMillis;
    stream >> record.critical;
    stream >> record.actualStart;
    stream >> record.actualFinish;
    stream >> record.actualDurationMillis;
    stream >> record.actualWorkMillis;
    stream >> record.evm;
    stream >> record.cost;
    stream >> record.fixedCost;
    stream >> record.fixedCostAccrual;
    stream >> record.actualCost;
    stream >> record.remainingCost;
    stream >> record.costVariance;
    stream >> record.startVarianceMillis;
    stream >> record.finishVarianceMillis;
    stream >> record.durationVarianceMillis;
    stream >> record.workVarianceMillis;
    stream >> record.rowFormat;
    stream >> record.barColor;
    stream >> record.cellFormats;
    stream >> record.baselines;
    stream >> record.customFields;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const Resource &record)
{
    stream << record.uniqueId;
    stream << record.id;
    stream << record.name;
    stream << record.initials;
    stream << record.type;
    stream << record.budget;
    stream << record.materialLabel;
    stream << record.maxUnits;
    stream << record.notes;
    stream << record.calendarUniqueId;
    stream << record.cost;
    stream << record.actualCost;
    stream << record.remainingCost;
    stream << record.costVariance;
    stream << record.budgetCost;
    stream << record.budgetWorkMillis;
    stream << record.baselines;
    stream << record.customFields;
    stream << record.costRates;
    stream << record.availabilityTable;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, Resource &record)
{
    stream >> record.uniqueId;
    stream >> record.id;
    stream >> record.name;
    stream >> record.initials;
    stream >> record.type;
    stream >> record.budget;
    stream >> record.materialLabel;
    stream >> record.maxUnits;
    stream >> record.notes;
    stream >> record.calendarUniqueId;
    stream >> record.cost;
    stream >> record.actualCost;
    stream >> record.remainingCost;
    stream >> record.costVariance;
    stream >> record.budgetCost;
    stream >> record.budgetWorkMillis;
    stream >> record.baselines;
    stream >> record.customFields;
    stream >> record.costRates;
    stream >> record.availabilityTable;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const Assignment &record)
{
    stream << record.uniqueId;
    stream << record.taskUniqueId;
    stream << record.resourceUniqueId;
    stream << record.budget;
    stream << record.budgetCost;
    stream << record.budgetWorkMillis;
    stream << record.units;
    stream << record.costRateTable;
    stream << record.variableRateUnits;
    stream << record.workContour;
    stream << record.workMillis;
    stream << record.notes;
    stream << record.start;
    stream << record.finish;
    stream << record.stop;
    stream << record.resume;
    stream << record.delayMillis;
    stream << record.levelingDelayMillis;
    stream << record.actualWorkMillis;
    stream << record.remainingWorkMillis;
    stream << record.overtimeWorkMillis;
    stream << record.actualOvertimeWorkMillis;
    stream << record.remainingOvertimeWorkMillis;
    stream << record.cost;
    stream << record.actualCost;
    stream << record.remainingCost;
    stream << record.costVariance;
    stream << record.overtimeCost;
    stream << record.actualOvertimeCost;
    stream << record.remainingOvertimeCost;
    stream << record.baselines;
    stream << record.customFields;
    stream << record.timephasedValues;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, Assignment &record)
{
    stream >> record.uniqueId;
    stream >> record.taskUniqueId;
    stream >> record.resourceUniqueId;
    stream >> record.budget;
    stream >> record.budgetCost;
    stream >> record.budgetWorkMillis;
    stream >> record.units;
    stream >> record.costRateTable;
    stream >> record.variableRateUnits;
    stream >> record.workContour;
    stream >> record.workMillis;
    stream >> record.notes;
    stream >> record.start;
    stream >> record.finish;
    stream >> record.stop;
    stream >> record.resume;
    stream >> record.delayMillis;
    stream >> record.levelingDelayMillis;
    stream >> record.actualWorkMillis;
    stream >> record.remainingWorkMillis;
    stream >> record.overtimeWorkMillis;
    stream >> record.actualOvertimeWorkMillis;
    stream >> record.remainingOvertimeWorkMillis;
    stream >> record.cost;
    stream >> record.actualCost;
    stream >> record.remainingCost;
    stream >> record.costVariance;
    stream >> record.overtimeCost;
    stream >> record.actualOvertimeCost;
    stream >> record.remainingOvertimeCost;
    stream >> record.baselines;
    stream >> record.customFields;
    stream >> record.timephasedValues;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const CustomField::IndicatorRule &record)
{
    stream << record.comparison;
    stream << record.value;
    stream << record.indicator;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, CustomField::IndicatorRule &record)
{
    stream >> record.comparison;
    stream >> record.value;
    stream >> record.indicator;
    return stream;
}

static QDataStream &operator<<(QDataStream &stream, const Project &record)
{
    stream << record.calendars;
    stream << record.relations;
    stream << record.customFieldDefinitions;
    stream << record.viewStyles;
    stream << record.resourceUsageStyles;
    stream << record.ganttView;
    stream << record.resourceUsageView;
    stream << record.taskUsageView;
    stream << record.teamPlannerView;
    stream << record.resourceSheetView;
    stream << record.calendarView;
    stream << record.networkView;
    stream << record.teamPlannerStyles;
    stream << record.calendarStyles;
    stream << record.timelineView;
    return stream;
}

static QDataStream &operator>>(QDataStream &stream, Project &record)
{
    stream >> record.calendars;
    stream >> record.relations;
    stream >> record.customFieldDefinitions;
    stream >> record.viewStyles;
    stream >> record.resourceUsageStyles;
    stream >> record.ganttView;
    stream >> record.resourceUsageView;
    stream >> record.taskUsageView;
    stream >> record.teamPlannerView;
    stream >> record.resourceSheetView;
    stream >> record.calendarView;
    stream >> record.networkView;
    stream >> record.teamPlannerStyles;
    stream >> record.calendarStyles;
    stream >> record.timelineView;
    return stream;
}


namespace {
constexpr quint32 recordRevision = 1;

template<class T>
QByteArray encode(const T &record)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_5_15);
    stream << recordRevision << record;
    return bytes;
}

template<class T>
bool decode(const QByteArray &bytes, T &record)
{
    QDataStream stream(bytes);
    stream.setVersion(QDataStream::Qt_5_15);
    quint32 revision = 0;
    stream >> revision;
    if (revision != recordRevision)
        return false;
    T decoded;
    stream >> decoded;
    if (stream.status() != QDataStream::Ok || !stream.atEnd())
        return false;
    record = std::move(decoded);
    return true;
}
} // namespace

QByteArray encodeVaultTask(const Task &record)
{
    return encode(record);
}

bool decodeVaultTask(const QByteArray &bytes, Task &record)
{
    return decode(bytes, record);
}

QByteArray encodeVaultResource(const Resource &record)
{
    return encode(record);
}

bool decodeVaultResource(const QByteArray &bytes, Resource &record)
{
    return decode(bytes, record);
}

QByteArray encodeVaultAssignment(const Assignment &record)
{
    return encode(record);
}

bool decodeVaultAssignment(const QByteArray &bytes, Assignment &record)
{
    return decode(bytes, record);
}

QByteArray encodeVaultProjectCollections(const Project &record)
{
    return encode(record);
}

bool decodeVaultProjectCollections(const QByteArray &bytes, Project &record)
{
    Project decoded;
    if (!decode(bytes, decoded))
        return false;
    record.calendars = std::move(decoded.calendars);
    record.relations = std::move(decoded.relations);
    record.customFieldDefinitions = std::move(decoded.customFieldDefinitions);
    record.viewStyles = std::move(decoded.viewStyles);
    record.resourceUsageStyles = std::move(decoded.resourceUsageStyles);
    record.ganttView = std::move(decoded.ganttView);
    record.resourceUsageView = std::move(decoded.resourceUsageView);
    record.taskUsageView = std::move(decoded.taskUsageView);
    record.teamPlannerView = std::move(decoded.teamPlannerView);
    record.resourceSheetView = std::move(decoded.resourceSheetView);
    record.calendarView = std::move(decoded.calendarView);
    record.networkView = std::move(decoded.networkView);
    record.teamPlannerStyles = std::move(decoded.teamPlannerStyles);
    record.calendarStyles = std::move(decoded.calendarStyles);
    record.timelineView = std::move(decoded.timelineView);
    return true;
}

} // namespace schedule
