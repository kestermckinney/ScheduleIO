// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// Print the decoded in-memory model of a .mpp (or .xml) at a glance: project
// header, tasks with hierarchy/format, resources, assignments, relations,
// calendars. The first stop when diffing decoded output against a fixture's
// XML export or manifest.
#include "mppio.h"
#include "xmlio.h"

#include <cstdio>

using schedule::Project;
using schedule::TextStyle;

static QString fmtStyle(const TextStyle &s)
{
    if (s.isDefault())
        return QString();
    QStringList bits;
    if (s.bold) bits << QStringLiteral("bold");
    if (s.italic) bits << QStringLiteral("italic");
    if (s.underline) bits << QStringLiteral("underline");
    if (s.strikethrough) bits << QStringLiteral("strike");
    if (s.color != TextStyle::kAutomatic)
        bits << QStringLiteral("color=#%1").arg(s.color, 6, 16, QLatin1Char('0'));
    if (s.backColor != TextStyle::kAutomatic)
        bits << QStringLiteral("back=#%1").arg(s.backColor, 6, 16, QLatin1Char('0'));
    if (s.backPattern != 0)
        bits << QStringLiteral("pattern=%1").arg(s.backPattern);
    return QStringLiteral("  [%1]").arg(bits.join(QLatin1Char(' ')));
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: dump_model <file.mpp|file.xml>\n");
        return 2;
    }
    const QString path = QString::fromLocal8Bit(argv[1]);
    Project p;
    if (path.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)) {
        XmlIO io;
        if (!io.open(path)) {
            fprintf(stderr, "read failed: %s\n", qPrintable(io.errorString()));
            return 1;
        }
        p = io.project();
    } else {
        MppIO io;
        if (!io.open(path)) {
            fprintf(stderr, "read failed: %s\n", qPrintable(io.errorString()));
            return 1;
        }
        p = io.project();
    }

    printf("title='%s' author='%s' start=%s finish=%s calUid=%d version=%d multipleCritical=%d budgetCost=%.2f budgetWork=%lldm\n",
           qPrintable(p.title), qPrintable(p.author),
           qPrintable(p.startDate.toString(Qt::ISODate)),
           qPrintable(p.finishDate.toString(Qt::ISODate)),
           p.calendarUniqueId, int(p.formatVersion), p.multipleCriticalPaths ? 1 : 0,
           p.budgetCost, (long long)(p.budgetWorkMillis / 60000));

    printf("opts: taskType=%d durUnits=%d workUnits=%d effortDriven=%d startOnCur=%d split=%d "
           "critSlack=%d weekStart=%d fyMonth=%d fyStartYear=%d startTime=%s endTime=%s "
           "minPerDay=%d minPerWeek=%d daysPerMonth=%d moveCEB=%d moveRSB=%d moveRSF=%d moveCEF=%d "
           "updRes=%d curSym='%s' curPos=%d curDig=%d curCode='%s' stdRate=%.2f otRate=%.2f "
           "accrual=%d evMethod=%d evBaseline=%d\n",
           p.defaultTaskType, p.defaultDurationUnits, p.defaultWorkUnits, p.newTasksEffortDriven ? 1 : 0,
           p.newTaskStartIsProjectStart ? 0 : 1, p.splitInProgressTasks ? 1 : 0,
           p.criticalSlackLimit, p.weekStartDay, p.fiscalYearStartMonth, p.fiscalYearUsesStartYear ? 1 : 0,
           qPrintable(p.defaultStartTime.toString(QStringLiteral("HH:mm"))),
           qPrintable(p.defaultEndTime.toString(QStringLiteral("HH:mm"))),
           p.minutesPerDay, p.minutesPerWeek, p.daysPerMonth,
           p.moveCompletedEndsBack ? 1 : 0, p.moveRemainingStartsBack ? 1 : 0,
           p.moveRemainingStartsForward ? 1 : 0, p.moveCompletedEndsForward ? 1 : 0,
           p.statusUpdatesResource ? 1 : 0, qPrintable(p.currencySymbol), p.currencySymbolPosition,
           p.currencyDigits, qPrintable(p.currencyCode), p.defaultStandardRate, p.defaultOvertimeRate,
           p.defaultFixedCostAccrual, p.defaultEarnedValueMethod, p.baselineForEarnedValue);

    printf("-- %lld tasks\n", (long long)p.tasks.size());
    for (const schedule::Task &t : p.tasks) {
        printf("  uid=%-3d id=%-3d L%d %s'%s'%s%s%s dur=%lldm %s..%s pct=%.0f\n",
               t.uniqueId, t.id, t.outlineLevel,
               QByteArray(2 * (t.outlineLevel - 1), ' ').constData(),
               qPrintable(t.name),
               t.summary ? " [sum]" : "", t.milestone ? " [mile]" : "",
               qPrintable(fmtStyle(t.rowFormat)),
               (long long)(t.durationMillis / 60000),
               qPrintable(t.start.toString(Qt::ISODate)),
               qPrintable(t.finish.toString(Qt::ISODate)),
               t.percentComplete);
        for (const schedule::CustomField &field : t.customFields)
            printf("      custom id=%d name='%s' value='%s' formula='%s' lookup=%lld indicators=%lld\n",
                   field.fieldId, qPrintable(field.name), qPrintable(field.value.toString()),
                   qPrintable(field.formula), (long long)field.lookupValues.size(),
                   (long long)field.graphicalIndicators.size());
        for (const schedule::TaskSegment &segment : t.segments)
            printf("      segment %s..%s\n", qPrintable(segment.start.toString(Qt::ISODate)),
                   qPrintable(segment.finish.toString(Qt::ISODate)));
    }

    printf("-- %lld resources\n", (long long)p.resources.size());
    for (const schedule::Resource &r : p.resources)
        printf("  uid=%-3d '%s' type=%d budget=%d budgetCost=%.2f budgetWork=%lldm maxUnits=%.2f calUid=%d costRates=%lld\n",
               r.uniqueId, qPrintable(r.name), int(r.type), r.budget ? 1 : 0, r.budgetCost,
               (long long)(r.budgetWorkMillis / 60000), r.maxUnits,
               r.calendarUniqueId, (long long)r.costRates.size());

    printf("-- %lld assignments\n", (long long)p.assignments.size());
    for (const schedule::Assignment &a : p.assignments) {
        printf("  task=%-3d res=%-3d units=%.2f work=%lldm cost=%.2f budget=%d budgetCost=%.2f budgetWork=%lldm timephased=%lld\n",
               a.taskUniqueId, a.resourceUniqueId, a.units,
               (long long)(a.workMillis / 60000), a.cost, a.budget ? 1 : 0,
               a.budgetCost, (long long)(a.budgetWorkMillis / 60000),
               (long long)a.timephasedValues.size());
        for (const schedule::TimephasedValue &value : a.timephasedValues)
            printf("      timephased type=%d baseline=%d %s..%s value='%s'\n",
                   value.type, value.baselineNumber,
                   qPrintable(value.start.toString(Qt::ISODate)),
                   qPrintable(value.finish.toString(Qt::ISODate)), qPrintable(value.value));
    }

    printf("-- %lld relations\n", (long long)p.relations.size());
    for (const schedule::Relation &r : p.relations)
        printf("  %d -> %d type=%d lag=%lldm\n", r.predecessorTaskUid,
               r.successorTaskUid, int(r.type), (long long)(r.lagMillis / 60000));

    printf("-- %lld calendars\n", (long long)p.calendars.size());
    for (const schedule::Calendar &c : p.calendars) {
        printf("  uid=%-3d '%s' base=%d mask=0x%02x exceptions=%lld\n",
               c.uniqueId, qPrintable(c.name), c.baseCalendarUniqueId,
               unsigned(c.workingDayMask), (long long)c.exceptions.size());
        for (const schedule::CalendarException &exception : c.exceptions)
            printf("      exception '%s' %s..%s recurrence=%d interval=%d weekdays=0x%02x occurrences=%d\n",
                   qPrintable(exception.name), qPrintable(exception.fromDate.toString(Qt::ISODate)),
                   qPrintable(exception.toDate.toString(Qt::ISODate)), int(exception.recurrence),
                   exception.interval, unsigned(exception.weekDayMask), exception.occurrences);
    }

    return 0;
}
